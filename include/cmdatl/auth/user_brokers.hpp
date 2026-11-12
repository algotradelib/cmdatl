#pragma once

// A broker or data vendor cmdatl was not built for: a folder the user keeps where they like (in a project, or in a git
// submodule of one), named by its path: `bars.adapter` in a run spec, `account.adapter` in the operator's file,
// `--adapter` for `auth login`.
//
// The folder holds `broker.toml` (what `auth login` asks) and `adapter.hpp` (the C++ a run's or a backtest's driver
// compiles). It is native code: the checks here keep other users from replacing it (every file and directory in it
// owned by the user and not writable by group or others, no link; every directory on the way to it owned by the user
// or root, and not writable by others unless its sticky bit is set), they do not confine it. A built-in name cannot be
// taken.

#include "../core/reply.hpp"
#include "../core/runs.hpp"
#include "../core/toml.hpp"
#include "../system/files.hpp"
#include "./brokers.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <sys/acl.h>
#endif

namespace cmdatl {
namespace auth {

/** @brief The most bytes `broker.toml` may hold. */
inline constexpr std::size_t MAX_BROKER_TOML_BYTES = 64u << 10;
/** @brief The most entries (files and directories) a broker folder may hold. */
inline constexpr std::size_t MAX_ADAPTER_ENTRIES = 1000;
/** @brief The deepest a broker folder may nest. */
inline constexpr int MAX_ADAPTER_DEPTH = 8;
/** @brief The most bytes a broker folder's files may hold together. */
inline constexpr std::uint64_t MAX_ADAPTER_BYTES = 16ull << 20;
/** @brief The most fields a login may have. */
inline constexpr std::size_t MAX_BROKER_FIELDS = 16;

/** @brief What finding a broker by name found. */
enum class BrokerFound {
  /** @brief A built-in broker. */
  BUILT_IN,
  /** @brief A broker folder, read and trusted. */
  FOLDER,
  /** @brief Neither: no built-in broker of that name, or no folder at the path. */
  NOT_FOUND,
  /** @brief A folder whose `broker.toml` cannot be read or is refused. */
  INVALID,
  /** @brief A folder another user could change. */
  UNSAFE,
};

/** @brief A broker folder's files as read once: what a driver's build compiles, whatever the folder holds later. */
struct AdapterSnapshot {
  /** @brief The broker's name. */
  std::string name_;
  /** @brief The folder, absolute and canonical. */
  std::string folder_;
  /** @brief Each file's path relative to the folder, and its bytes, sorted by path. */
  std::vector<std::pair<std::string, std::string>> files_;
};

/** @brief A broker folder, read. */
struct UserBroker {
  /** @brief Its entry, as `auth login` reads it; `read_` is null: the document is stored unchecked. */
  Broker entry_;
  /** @brief The folder, absolute and canonical: what a login is bound to. */
  std::filesystem::path folder_;
  /** @brief A market-data vendor: a bar source, never an account. */
  bool data_only_{false};
  /** @brief Its files, read once with `broker.toml`: the bytes checked are the bytes compiled. */
  AdapterSnapshot snapshot_;
};

/** @brief A broker found by name and, for a broker folder, its path. */
struct BrokerLookup {
  /** @brief What was found. */
  BrokerFound found_{BrokerFound::NOT_FOUND};
  /** @brief The built-in broker, for `BUILT_IN`. */
  const Broker* built_in_{nullptr};
  /** @brief The folder, for `FOLDER`. */
  std::optional<UserBroker> folder_;
  /** @brief Why not, for `NOT_FOUND`, `INVALID` and `UNSAFE`: a path and a rule, never a file's content. */
  std::string problem_;

  /**
   * @brief The broker's entry.
   * @return It, or null when none was found.
   */
  const Broker* broker() const {
    return built_in_ != nullptr ? built_in_ : folder_ ? &folder_->entry_ : nullptr;
  }
};

/** @brief A file of a broker folder. */
struct AdapterFile {
  /** @brief Its path relative to the folder. */
  std::string relative_;
  /** @brief Its size when it was checked. */
  std::uint64_t size_{0};
};

#if !defined(_WIN32)
/**
 * @brief Whether an entry carries an access control list that allows anything: on macOS an ACL can grant another user
 *   write access while the mode bits show none (on Linux a POSIX ACL's grants show in the group bits). Deny entries are
 *   harmless; any allow entry, or an ACL that cannot be read, counts.
 * @param path The entry, not followed through a link.
 * @return Whether it does.
 */
inline bool acl_allows(const std::filesystem::path& path) {
#if defined(__APPLE__)
  errno = 0;
  acl_t acl = ::acl_get_link_np(path.c_str(), ACL_TYPE_EXTENDED);
  if (acl == nullptr) {
    return errno != ENOENT;
  }
  bool allows = false;
  acl_entry_t entry = nullptr;
  for (int id = ACL_FIRST_ENTRY; ::acl_get_entry(acl, id, &entry) == 0; id = ACL_NEXT_ENTRY) {
    acl_tag_t tag{};
    if (::acl_get_tag_type(entry, &tag) != 0 || tag != ACL_EXTENDED_DENY) {
      allows = true;
      break;
    }
  }
  ::acl_free(acl);
  return allows;
#else
  (void)path;
  return false;
#endif
}

/**
 * @brief Why an entry of a broker folder cannot be trusted: not the user's, writable by group or others, an access
 *   control list allowing access, or not the kind expected (a link, a device, a FIFO).
 * @param path The entry, for the message.
 * @param info Its `lstat`.
 * @param directory Whether a directory is expected (else a regular file).
 * @return The reason, or none.
 */
inline std::optional<std::string> untrusted(
  const std::filesystem::path& path, const struct stat& info, bool directory
) {
  if (directory ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode)) {
    return path.string() + " is not a " + (directory ? "directory" : "regular file") + " (links are not followed)";
  }
  if (info.st_uid != ::geteuid()) {
    return path.string() + " is not owned by this user";
  }
  if ((info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    return path.string() + " is writable by its group or others";
  }
  if (acl_allows(path)) {
    return path.string() + " has an access control list allowing access (ls -le shows it; chmod -N removes it)";
  }
  return std::nullopt;
}

/**
 * @brief Checks every directory on the way to a broker folder, as the path is written (never simplified, so a link
 *   before a `..` is seen): each is owned by the user or root, and is not writable by its group or others unless its
 *   sticky bit is set, nor through an access control list; a link on the way is owned by the user or root, and the
 *   directories it leads to are checked when the canonical path is.
 * @param folder The folder, absolute; the folder itself is not checked here.
 * @return Why another user could replace the folder, or none.
 */
inline std::optional<std::string> unsafe_way(const std::filesystem::path& folder) {
  std::filesystem::path at;
  for (const auto& part : folder.parent_path()) {
    at /= part;
    struct stat info{};
    if (::lstat(at.c_str(), &info) != 0) {
      return at.string() + " cannot be read";
    }
    const bool owned = info.st_uid == ::geteuid() || info.st_uid == 0;
    if (!owned) {
      return at.string() + " belongs to another user, who could replace what is in it";
    }
    if (S_ISLNK(info.st_mode)) {
      continue;
    }
    if (!S_ISDIR(info.st_mode)) {
      return at.string() + " is not a directory";
    }
    const bool sticky = (info.st_mode & S_ISVTX) != 0;
    if ((info.st_mode & (S_IWGRP | S_IWOTH)) != 0 && !sticky) {
      return at.string() + " is writable by its group or others, who could replace what is in it";
    }
    if (acl_allows(at)) {
      return at.string() + " has an access control list allowing access (ls -le shows it; chmod -N removes it)";
    }
  }
  return std::nullopt;
}

/**
 * @brief Lists a broker folder's files, checking every directory and file on the way, within the bounds.
 * @param folder The folder.
 * @param files Set to its files, sorted by relative path.
 * @param problem Why it was refused.
 * @return `FOLDER`, `INVALID` (beyond a bound, unreadable) or `UNSAFE`.
 */
inline BrokerFound list_folder(
  const std::filesystem::path& folder, std::vector<AdapterFile>& files, std::string& problem
) {
  files.clear();
  std::size_t entries = 0;
  std::uint64_t bytes = 0;
  // Directories still to read: (absolute path, relative path, depth).
  struct Pending {
    std::filesystem::path path_;
    std::string relative_;
    int depth_;
  };
  std::vector<Pending> pending{{folder, "", 0}};
  while (!pending.empty()) {
    const auto at = pending.back();
    pending.pop_back();
    const int fd = open_lifted(at.path_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
      problem = at.path_.string() + " cannot be opened as a directory (links are not followed)";
      return BrokerFound::UNSAFE;
    }
    DIR* dir = ::fdopendir(fd);
    if (dir == nullptr) {
      ::close(fd);
      problem = at.path_.string() + " cannot be read";
      return BrokerFound::INVALID;
    }
    struct stat self{};
    if (::fstat(fd, &self) != 0) {
      ::closedir(dir);
      problem = at.path_.string() + " cannot be read";
      return BrokerFound::INVALID;
    }
    if (const auto reason = untrusted(at.path_, self, true)) {
      ::closedir(dir);
      problem = *reason;
      return BrokerFound::UNSAFE;
    }
    std::vector<std::string> names;
    while (const auto* entry = ::readdir(dir)) {
      const std::string name(entry->d_name);
      if (name != "." && name != "..") {
        names.push_back(name);
      }
    }
    for (const auto& name : names) {
      if (++entries > MAX_ADAPTER_ENTRIES) {
        ::closedir(dir);
        problem = folder.string() + " holds more than " + std::to_string(MAX_ADAPTER_ENTRIES) +
          " entries: keep only the adapter's own files in it";
        return BrokerFound::INVALID;
      }
      const auto path = at.path_ / name;
      const auto relative = at.relative_.empty() ? name : at.relative_ + "/" + name;
      struct stat info{};
      if (::fstatat(fd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        ::closedir(dir);
        problem = path.string() + " cannot be read";
        return BrokerFound::INVALID;
      }
      const bool directory = S_ISDIR(info.st_mode);
      if (const auto reason = untrusted(path, info, directory)) {
        ::closedir(dir);
        problem = *reason;
        return BrokerFound::UNSAFE;
      }
      if (directory) {
        if (at.depth_ + 1 > MAX_ADAPTER_DEPTH) {
          ::closedir(dir);
          problem = path.string() + " is nested deeper than " + std::to_string(MAX_ADAPTER_DEPTH) + " levels";
          return BrokerFound::INVALID;
        }
        pending.push_back({path, relative, at.depth_ + 1});
        continue;
      }
      bytes += static_cast<std::uint64_t>(info.st_size);
      if (bytes > MAX_ADAPTER_BYTES) {
        ::closedir(dir);
        problem = folder.string() + " holds more than " + std::to_string(MAX_ADAPTER_BYTES >> 20) +
          " MiB: keep only the adapter's own files in it";
        return BrokerFound::INVALID;
      }
      files.push_back({relative, static_cast<std::uint64_t>(info.st_size)});
    }
    ::closedir(dir);
  }
  std::sort(files.begin(), files.end(), [](const AdapterFile& a, const AdapterFile& b) {
    return a.relative_ < b.relative_;
  });
  return BrokerFound::FOLDER;
}
#endif

/**
 * @brief Whether a text holds no control character: it goes to a terminal as a prompt or a name.
 * @param text The text.
 * @return Whether it holds none.
 */
inline bool printable(const std::string& text) {
  return std::none_of(text.begin(), text.end(), [](char c) {
    const auto u = static_cast<unsigned char>(c);
    return u < 0x20 || u == 0x7f;
  });
}

/**
 * @brief Reads a folder's `broker.toml` into an entry, strictly: an unknown key, a wrong type, an empty or
 *   unprintable string, or a rule broken is refused with its line and column.
 * @param name The broker's name.
 * @param text The file's text.
 * @param out The broker read.
 * @return The refusal's message, naming the line and column, or none.
 */
inline std::optional<std::string> read_broker_toml(const std::string& name, const std::string& text, UserBroker& out) {
  using config_detail::Refusal;
  using config_detail::refuse;
  using config_detail::type_name;
  const auto at = [](const Refusal& refusal) {
    return std::to_string(refusal.at_.line) + ":" + std::to_string(refusal.at_.column) + ": " + refusal.message_;
  };
  if (auto deep = config_detail::nesting_refusal(text)) {
    return at(*deep);
  }
  if (auto foreign = config_detail::non_ascii_refusal(text)) {
    return at(*foreign);
  }
  toml::table root;
  try {
    root = toml::parse(std::string_view(text));
  } catch (const TomlInvariant& error) {
    return std::string("not TOML this version reads (") + error.what() + ")";
  } catch (const toml::parse_error& error) {
    return std::to_string(error.source().begin.line) + ":" + std::to_string(error.source().begin.column) +
      ": not valid TOML: " + std::string(error.description());
  }
  if (auto bad = config_detail::only_keys(root, "", {"display", "demo", "data_only", "fields"})) {
    return at(*bad);
  }
  const auto text_of =
    [&](
      const toml::table& table, const char* key, const std::string& label, bool required,
      std::optional<std::string>& value
    ) -> std::optional<Refusal> {
    if (auto bad = config_detail::string_of(table, key, label, value)) {
      return bad;
    }
    if (!value) {
      if (required) {
        return refuse(table.source(), label + " is needed");
      }
      return std::nullopt;
    }
    if (value->empty() || value->size() > 200 || !printable(*value)) {
      return refuse(table.get(key)->source(), label + " must be 1 to 200 printable characters");
    }
    return std::nullopt;
  };
  const auto flag_of =
    [&](const toml::table& table, const char* key, const std::string& label, bool& value) -> std::optional<Refusal> {
    const auto* node = table.get(key);
    if (node == nullptr) {
      return std::nullopt;
    }
    if (node->as_boolean() == nullptr) {
      return refuse(node->source(), label + " must be true or false, not " + type_name(*node));
    }
    value = node->as_boolean()->get();
    return std::nullopt;
  };
  UserBroker broker;
  broker.entry_.name_ = name;
  std::optional<std::string> display;
  if (auto bad = text_of(root, "display", "display", true, display)) {
    return at(*bad);
  }
  broker.entry_.display_ = *display;
  if (auto bad = flag_of(root, "demo", "demo", broker.entry_.has_demo_)) {
    return at(*bad);
  }
  if (auto bad = flag_of(root, "data_only", "data_only", broker.data_only_)) {
    return at(*bad);
  }
  const auto* fields = root.get("fields");
  if (fields == nullptr) {
    return at(refuse(root.source(), "the file needs [[fields]], what a login asks"));
  }
  const auto* list = fields->as_array();
  if (list == nullptr || list->empty() || list->size() > MAX_BROKER_FIELDS) {
    return at(
      refuse(fields->source(), "fields must be 1 to " + std::to_string(MAX_BROKER_FIELDS) + " [[fields]] tables")
    );
  }
  std::set<std::string> seen;
  for (const auto& node : *list) {
    const auto* field = node.as_table();
    if (field == nullptr) {
      return at(refuse(node.source(), "each of fields must be a table, not " + type_name(node)));
    }
    if (auto bad = config_detail::only_keys(*field, "fields", {"name", "prompt", "secret", "default"})) {
      return at(*bad);
    }
    BrokerField out;
    std::optional<std::string> value;
    if (auto bad = text_of(*field, "name", "fields.name", true, value)) {
      return at(*bad);
    }
    const auto& key = *value;
    const bool identifier = key.size() <= 64 &&
      ((key[0] >= 'A' && key[0] <= 'Z') || (key[0] >= 'a' && key[0] <= 'z') || key[0] == '_') &&
      std::all_of(key.begin(), key.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
      });
    if (!identifier || key == "demo") {
      return at(refuse(
        field->get("name")->source(),
        "fields.name must be a letter or '_', then letters, digits or '_' (64 at most), "
        "and not demo"
      ));
    }
    if (!seen.insert(key).second) {
      return at(refuse(field->get("name")->source(), "fields.name " + key + " is given twice"));
    }
    out.json_ = key;
    value = std::nullopt;
    if (auto bad = text_of(*field, "prompt", "fields.prompt", true, value)) {
      return at(*bad);
    }
    out.prompt_ = *value;
    if (auto bad = flag_of(*field, "secret", "fields.secret", out.secret_)) {
      return at(*bad);
    }
    value = std::nullopt;
    if (auto bad = text_of(*field, "default", "fields.default", false, value)) {
      return at(*bad);
    }
    if (out.secret_ && value) {
      return at(refuse(field->source(), "a secret field has no default: remove it"));
    }
    out.default_ = value;
    broker.entry_.fields_.push_back(std::move(out));
  }
  out = std::move(broker);
  return std::nullopt;
}

/**
 * @brief Reads a broker folder's files, each checked again through the descriptor it is read from, within the bounds.
 * @param folder The folder, canonical.
 * @param out The files read.
 * @param problem Why not.
 * @return `FOLDER`, `INVALID` or `UNSAFE`.
 */
inline BrokerFound read_folder(const std::filesystem::path& folder, AdapterSnapshot& out, std::string& problem) {
#if defined(_WIN32)
  (void)folder;
  (void)out;
  problem = "broker folders are read on macOS and Linux only in this version";
  return BrokerFound::INVALID;
#else
  std::vector<AdapterFile> files;
  const auto listed = list_folder(folder, files, problem);
  if (listed != BrokerFound::FOLDER) {
    return listed;
  }
  out.folder_ = folder.string();
  out.files_.clear();
  std::uint64_t total = 0;
  for (const auto& file : files) {
    const auto path = folder / file.relative_;
    ScopedFd fd(open_lifted(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    struct stat info{};
    if (fd.get() < 0 || ::fstat(fd.get(), &info) != 0) {
      problem = path.string() + " cannot be opened";
      return BrokerFound::INVALID;
    }
    if (const auto reason = untrusted(path, info, false)) {
      problem = *reason;
      return BrokerFound::UNSAFE;
    }
    std::string bytes;
    const auto read = read_whole(fd.get(), static_cast<std::size_t>(MAX_ADAPTER_BYTES - total), bytes);
    if (read.status_ != FileRead::Status::OK) {
      problem = path.string() + " was not read: " + read.text(static_cast<std::size_t>(MAX_ADAPTER_BYTES - total));
      return BrokerFound::INVALID;
    }
    total += bytes.size();
    out.files_.emplace_back(file.relative_, std::move(bytes));
  }
  return BrokerFound::FOLDER;
#endif
}

/**
 * @brief Finds a broker: a built-in one by name, else the broker folder at a path, checked (the directories on the way
 *   to it, then the folder's tree) and read once: `broker.toml` is read from the same snapshot that is compiled.
 * @param name The name.
 * @param adapter The folder's path, absolute and as written, for a name that is not built-in; empty for a built-in one.
 * @return What was found.
 */
inline BrokerLookup lookup_broker(const std::string& name, const std::string& adapter) {
  BrokerLookup found;
  if (const auto* built_in = find_broker(name)) {
    if (!adapter.empty()) {
      found.found_ = BrokerFound::INVALID;
      found.problem_ = name + " is a built-in broker: a broker folder is named for another name only";
      return found;
    }
    found.found_ = BrokerFound::BUILT_IN;
    found.built_in_ = built_in;
    return found;
  }
  if (!user_broker_name(name)) {
    found.problem_ = "no broker " + name;
    return found;
  }
  if (adapter.empty()) {
    found.problem_ = name + " is not a built-in broker, and no broker folder is named for it";
    return found;
  }
#if defined(_WIN32)
  found.problem_ = "broker folders are read on macOS and Linux only in this version";
  return found;
#else
  std::string written = adapter;
  while (written.size() > 1 && written.back() == '/') {
    written.pop_back();
  }
  const std::filesystem::path given(written);
  struct stat info{};
  if (::lstat(given.c_str(), &info) != 0) {
    found.problem_ = "no broker folder " + given.string();
    return found;
  }
  if (auto reason = untrusted(given, info, true)) {
    found.found_ = BrokerFound::UNSAFE;
    found.problem_ = std::move(*reason);
    return found;
  }
  std::error_code ec;
  const auto folder = std::filesystem::canonical(given, ec);
  if (ec) {
    found.found_ = BrokerFound::INVALID;
    found.problem_ = given.string() + " cannot be resolved";
    return found;
  }
  // The path as written, then where it leads: a link on the way is followed only into directories that are checked.
  for (const auto& path : {given, folder}) {
    if (auto reason = unsafe_way(path)) {
      found.found_ = BrokerFound::UNSAFE;
      found.problem_ = std::move(*reason);
      return found;
    }
  }
  UserBroker broker;
  const auto read = read_folder(folder, broker.snapshot_, found.problem_);
  if (read != BrokerFound::FOLDER) {
    found.found_ = read;
    return found;
  }
  const auto& files = broker.snapshot_.files_;
  const auto file = [&files](const std::string& relative) -> const std::string* {
    for (const auto& entry : files) {
      if (entry.first == relative) {
        return &entry.second;
      }
    }
    return nullptr;
  };
  const auto* toml = file("broker.toml");
  if (toml == nullptr || file("adapter.hpp") == nullptr) {
    found.found_ = BrokerFound::INVALID;
    found.problem_ = folder.string() + " needs broker.toml and adapter.hpp";
    return found;
  }
  const auto path = folder / "broker.toml";
  if (toml->size() > MAX_BROKER_TOML_BYTES) {
    found.found_ = BrokerFound::INVALID;
    found.problem_ = path.string() + " holds more than " + std::to_string(MAX_BROKER_TOML_BYTES >> 10) + " KiB";
    return found;
  }
  auto snapshot = std::move(broker.snapshot_);
  if (const auto problem = read_broker_toml(name, *toml, broker)) {
    found.found_ = BrokerFound::INVALID;
    found.problem_ = path.string() + ":" + *problem;
    return found;
  }
  snapshot.name_ = name;
  broker.folder_ = folder;
  broker.snapshot_ = std::move(snapshot);
  found.found_ = BrokerFound::FOLDER;
  found.folder_ = std::move(broker);
  return found;
#endif
}

/**
 * @brief The refusal for a broker that found no usable folder.
 * @param found What was found: `NOT_FOUND`, `INVALID` or `UNSAFE`.
 * @return `adapter.not_found`, `adapter.invalid` or `adapter.unsafe` (exit 1).
 */
inline Reply broker_refusal(const BrokerLookup& found) {
  switch (found.found_) {
    case BrokerFound::INVALID:
      return error_reply(Exit::FAILED, "adapter.invalid", found.problem_, "the operator fixes the broker folder");
    case BrokerFound::UNSAFE:
      return error_reply(
        Exit::FAILED, "adapter.unsafe", found.problem_ + "; another user could change the code cmdatl would compile",
        "the operator makes the folder, and each directory on the way to it, theirs and not writable by others "
        "(chmod go-w)"
      );
    default:
      return error_reply(
        Exit::FAILED, "adapter.not_found", found.problem_,
        "the brokers are " + broker_names() +
          "; another is a broker folder named by its path (bars.adapter in a run spec, account.adapter in the "
          "operator's file)"
      );
  }
}

} // namespace auth
} // namespace cmdatl
