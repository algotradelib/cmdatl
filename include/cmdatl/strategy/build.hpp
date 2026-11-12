#pragma once

// Building a driver: one executable per program shape and kind (a run's driver, or a backtest's), in a cache directory,
// from the sources cmdatl carries (`cmdatl/driver_files.hpp`) and the program's `atlc build` output, as CmdStan compiles
// stanc's header.
//
// The shape's directory is named by what CMake fixes at configure time and does not track afterwards (the program's
// path, its indicators setting, the environment CMake reads, the algotradelib package cmdatl was built against, its
// market-price rule, and for a backtest its kind); everything else is CMake's own dependency tracking. Its lock is held from `atlc build` until the
// executable has been copied out (into the run's directory, or a backtest's scratch directory), so a driver always
// executes the build it asked for, and a backtest's replay never holds the lock.

#include "../account/account.hpp"
#include "../auth/user_brokers.hpp"
#include "../core/reply.hpp"
#include "../system/files.hpp"
#include "../system/process.hpp"
#include "./strategy.hpp"

#include <cmdatl/driver_files.hpp>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if !defined(CMDATL_ALGOTRADELIB_DIR)
#define CMDATL_ALGOTRADELIB_DIR ""
#endif

extern char** environ;

namespace cmdatl {
namespace build {

/** @brief The environment CMake reads at configure time, part of a shape's key and passed to the build unchanged. */
inline const std::vector<std::string> KEY_VARIABLES{
  "CXX",
  "CXXFLAGS",
  "CPPFLAGS",
  "LDFLAGS",
  "CMAKE_GENERATOR",
  "CMAKE_TOOLCHAIN_FILE",
  "OPENSSL_ROOT_DIR",
  "BOOST_ROOT",
  "Boost_ROOT",
  "Eigen3_ROOT",
};

/** @brief How often a build waiting on another build of the same shape looks again. */
inline constexpr std::chrono::milliseconds SHAPE_LOCK_STEP{20};

/**
 * @brief The cache's root: `cache_folder("drivers")`.
 * @return The root, or nothing.
 */
inline std::optional<std::string> cache_root() {
  return cache_folder("drivers");
}

/**
 * @brief FNV-1a, 64 bits.
 * @param text The bytes.
 * @return The hash.
 */
inline std::uint64_t fnv1a(const std::string& text) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const char c : text) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

/**
 * @brief A shape's directory name: the program's struct and 16 hex digits of the key, a length-prefixed encoding of the
 *   program's path, its indicators setting, each key variable (set or not), the algotradelib package directory, and
 *   any further fields (a backtest's kind and DuckDB's location, and the market-price rule's header and function).
 * @param program The program's absolute path.
 * @param structure The program's struct.
 * @param indicators The indicators setting.
 * @param lookup Reads an environment variable.
 * @param package The algotradelib package directory the driver is built against.
 * @param extra The further fields.
 * @return The name.
 */
inline std::string shape_name(
  const std::string& program, const std::string& structure, const std::string& indicators,
  const std::function<std::optional<std::string>(const std::string&)>& lookup, const std::string& package,
  const std::vector<std::string>& extra = {}
) {
  std::string encoded;
  const auto put = [&encoded](const std::string& field) {
    encoded += std::to_string(field.size()) + ":" + field + ";";
  };
  put(program);
  put(indicators);
  for (const auto& name : KEY_VARIABLES) {
    const auto value = lookup(name);
    put(name);
    put(value ? "=" + *value : "-");
  }
  put(package);
  for (const auto& field : extra) {
    put(field);
  }
  char hex[17];
  std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(fnv1a(encoded)));
  return structure + "-" + hex;
}

/**
 * @brief The environment a build's children run with: cmdatl's own, without the locators the key does not hold
 *   (`CMAKE_PREFIX_PATH` and every other `*_ROOT` or `*_DIR` variable), so a package's dependencies are never found
 *   through a variable that does not name the shape.
 * @param entries The environment, `NAME=VALUE`.
 * @param kept Further variables the shape's key holds, kept too.
 * @return The child's environment.
 */
inline std::vector<std::string> child_environment(
  const std::vector<std::string>& entries, const std::vector<std::string>& kept = {}
) {
  std::vector<std::string> out;
  for (const auto& entry : entries) {
    const auto name = entry.substr(0, entry.find('='));
    const bool keyed = std::find(KEY_VARIABLES.begin(), KEY_VARIABLES.end(), name) != KEY_VARIABLES.end() ||
      std::find(kept.begin(), kept.end(), name) != kept.end();
    const auto ends = [&name](const std::string& suffix) {
      return name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (!keyed && (name == "CMAKE_PREFIX_PATH" || ends("_ROOT") || ends("_DIR"))) {
      continue;
    }
    out.push_back(entry);
  }
  return out;
}

/**
 * @brief cmdatl's own environment, `NAME=VALUE`.
 * @return The entries.
 */
inline std::vector<std::string> own_environment() {
  std::vector<std::string> out;
#if !defined(_WIN32)
  for (char** at = environ; at != nullptr && *at != nullptr; ++at) {
    out.emplace_back(*at);
  }
#endif
  return out;
}

#if !defined(_WIN32)
/**
 * @brief Reads a whole regular file, not followed through a link.
 * @param path The file.
 * @param text Set to its bytes.
 * @return Whether it was read.
 */
inline bool read_file(const std::filesystem::path& path, std::string& text) {
  ScopedFd fd(open_lifted(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  if (fd.get() < 0) {
    return false;
  }
  struct stat info{};
  if (::fstat(fd.get(), &info) != 0 || !S_ISREG(info.st_mode)) {
    return false;
  }
  return read_whole(fd.get(), static_cast<std::size_t>(info.st_size) + 1, text).status_ == FileRead::Status::OK;
}

/**
 * @brief A temporary's name beside a file: `.<name>.<pid>.<16 hex>.tmp`, so no fixed name can block a later write.
 * @param target The file.
 * @return The temporary's path.
 */
inline std::filesystem::path temporary_for(const std::filesystem::path& target) {
  std::uint64_t random = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  ScopedFd urandom(open_lifted("/dev/urandom", O_RDONLY | O_CLOEXEC));
  if (urandom.get() >= 0) {
    std::uint64_t bytes = 0;
    if (::read(urandom.get(), &bytes, sizeof bytes) == static_cast<ssize_t>(sizeof bytes)) {
      random ^= bytes;
    }
  }
  char tail[64];
  std::snprintf(
    tail, sizeof tail, ".%ld.%016llx.tmp", static_cast<long>(::getpid()), static_cast<unsigned long long>(random)
  );
  return target.parent_path() / ("." + target.filename().string() + tail);
}

/**
 * @brief Writes a file through a temporary created exclusively, then renamed over it.
 * @param target The file.
 * @param text The bytes.
 * @param mode The new file's mode.
 * @return 0, or the error number.
 */
inline int replace_file(const std::filesystem::path& target, const std::string& text, mode_t mode = 0600) {
  const auto temporary = temporary_for(target);
  ScopedFd fd(open_lifted(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode));
  if (fd.get() < 0) {
    return errno;
  }
  std::size_t written = 0;
  if (const int error = write_whole(fd.get(), text.data(), text.size(), written)) {
    ::unlink(temporary.c_str());
    return error;
  }
  fd.reset();
  if (::rename(temporary.c_str(), target.c_str()) != 0) {
    const int error = errno;
    ::unlink(temporary.c_str());
    return error;
  }
  return 0;
}

/** @brief What `write_if_changed` did. */
enum class Written {
  /** @brief The file already held these bytes: untouched, so its modification time stays. */
  UNCHANGED,
  /** @brief The file was written. */
  REPLACED,
  /** @brief It could not be written. */
  FAILED,
};

/**
 * @brief Writes a file only when its bytes differ, so CMake rebuilds nothing for an unchanged one.
 * @param target The file; its missing parents are made `0700`.
 * @param text The bytes.
 * @return What happened.
 */
inline Written write_if_changed(const std::filesystem::path& target, const std::string& text) {
  std::string old;
  if (read_file(target, old) && old == text) {
    return Written::UNCHANGED;
  }
  if (make_private_dirs(target.parent_path()) != 0) {
    return Written::FAILED;
  }
  return replace_file(target, text) == 0 ? Written::REPLACED : Written::FAILED;
}

/**
 * @brief Removes the temporaries a killed build left in a directory tree: files named `.*.tmp`.
 * @param dir The directory.
 */
inline void remove_stale_temporaries(const std::filesystem::path& dir) {
  std::error_code ec;
  std::vector<std::filesystem::path> stale;
  for (
    auto it = std::filesystem::recursive_directory_iterator(dir, ec);
    !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)
  ) {
    const auto name = it->path().filename().string();
    if (name.size() > 5 && name.front() == '.' && name.compare(name.size() - 4, 4, ".tmp") == 0) {
      stale.push_back(it->path());
    }
  }
  for (const auto& path : stale) {
    std::filesystem::remove(path, ec);
  }
}

/**
 * @brief The files of a directory tree, by path relative to it.
 * @param dir The directory.
 * @return The relative paths of its regular files.
 */
inline std::set<std::string> files_under(const std::filesystem::path& dir) {
  std::set<std::string> out;
  std::error_code ec;
  for (
    auto it = std::filesystem::recursive_directory_iterator(dir, ec);
    !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)
  ) {
    if (it->is_regular_file(ec)) {
      out.insert(std::filesystem::relative(it->path(), dir, ec).generic_string());
    }
  }
  return out;
}

/** @brief A shape directory's lock: `flock` on its `lock` file, held while this lives. */
class ShapeLock {
public:
  /**
   * @brief Takes the lock, looking again every 20 ms; one progress line after the first refused try.
   * @param path The lock file.
   * @param progress Where the waiting line goes.
   * @return Whether it was taken (false when interrupted or the file cannot be opened).
   */
  bool take(const std::filesystem::path& path, const std::function<void(const std::string&)>& progress) {
    fd_.reset(open_lifted(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (fd_.get() < 0) {
      return false;
    }
    bool told = false;
    while (::flock(fd_.get(), LOCK_EX | LOCK_NB) != 0) {
      if (errno != EWOULDBLOCK && errno != EINTR) {
        fd_.reset();
        return false;
      }
      if (interrupted()) {
        fd_.reset();
        return false;
      }
      if (!told) {
        told = true;
        progress("cmdatl: waiting for another build of this program");
      }
      std::this_thread::sleep_for(SHAPE_LOCK_STEP);
    }
    return true;
  }

private:
  ScopedFd fd_;
};

/**
 * @brief Copies the built driver into a directory: `driver.tmp` created exclusively (mode 0700), every byte written,
 *   the count compared with the source's size, synced, then renamed to `driver`.
 * @param source The built executable.
 * @param run_dir The directory: the run's, or a backtest's scratch directory.
 * @return 0, or the error number (`EIO` for a short copy); a failed copy leaves no `driver.tmp`.
 */
inline int copy_driver(const std::filesystem::path& source, const std::filesystem::path& run_dir) {
  ScopedFd in(open_lifted(source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  if (in.get() < 0) {
    return errno;
  }
  struct stat info{};
  if (::fstat(in.get(), &info) != 0 || !S_ISREG(info.st_mode)) {
    return errno != 0 ? errno : EINVAL;
  }
  const auto temporary = run_dir / "driver.tmp";
  ScopedFd out(open_lifted(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0700));
  if (out.get() < 0) {
    return errno;
  }
  const auto fail = [&temporary](int error) {
    ::unlink(temporary.c_str());
    return error;
  };
  std::vector<char> buffer(1 << 16);
  std::uint64_t total = 0;
  while (true) {
    const auto got = ::read(in.get(), buffer.data(), buffer.size());
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      return fail(errno);
    }
    if (got == 0) {
      break;
    }
    std::size_t written = 0;
    if (const int error = write_whole(out.get(), buffer.data(), static_cast<std::size_t>(got), written)) {
      return fail(error);
    }
    total += static_cast<std::uint64_t>(got);
  }
  if (total != static_cast<std::uint64_t>(info.st_size)) {
    return fail(EIO);
  }
  if (::fsync(out.get()) != 0) {
    return fail(errno);
  }
  out.reset();
  if (::rename(temporary.c_str(), (run_dir / "driver").c_str()) != 0) {
    return fail(errno);
  }
  return 0;
}
#endif

/** @brief Which driver a build makes. */
enum class DriverKind {
  /** @brief A run's driver: cmdatl's own `driver/main.cpp`, on the mock market. */
  RUN,
  /** @brief A backtest's driver: `cmdatl::replay::main`, the replay module's own main. */
  BACKTEST,
};

/** @brief What a driver build needs. */
struct Request {
  /** @brief atlc's path. */
  std::string atlc_;
  /** @brief atlc's version, for messages. */
  std::string atlc_version_;
  /** @brief The program's absolute path. */
  std::string program_;
  /** @brief The program's C++ struct, inside `atls::`. */
  std::string struct_;
  /** @brief The indicators setting. */
  std::string indicators_;
  /** @brief The algotradelib package directory the driver is built against: the one cmdatl was built against. */
  std::string package_{CMDATL_ALGOTRADELIB_DIR};
  /** @brief Which driver. */
  DriverKind kind_{DriverKind::RUN};
  /** @brief Whether a backtest reads a store, so links DuckDB's C library. */
  bool store_{false};
  /** @brief DuckDB's directory (`build.duckdb`), absolute, or empty for `DUCKDB_ROOT`, then the system's paths. */
  std::string duckdb_{};
  /** @brief The market-price rule's header (`replay.fill_header`), absolute, or empty for the next bar's open. */
  std::string fill_header_{};
  /** @brief The market-price rule's function (`replay.fill_function`), a C++ name the header declares. */
  std::string fill_function_{};
  /** @brief The broker a run's driver trades (`kis`), or empty for the mock market or a broker folder. */
  std::string broker_{};
  /** @brief A broker folder's files, read once, compiled into the driver as `adapter/`; none for no folder. */
  std::optional<auth::AdapterSnapshot> adapter_{};
};

/**
 * @brief A broker folder's fingerprint: 16 hex digits of a hash of each file's path and bytes, each framed by its
 *   length. It tells one version of a folder from another for the build cache; it is not a signature.
 * @param snapshot The folder's files.
 * @return The fingerprint.
 */
inline std::string adapter_digest(const auth::AdapterSnapshot& snapshot) {
  std::string framed;
  for (const auto& [relative, bytes] : snapshot.files_) {
    framed += std::to_string(relative.size()) + ":" + relative + ";" + std::to_string(bytes.size()) + ":" + bytes + ";";
  }
  char digest[17];
  std::snprintf(digest, sizeof(digest), "%016llx", static_cast<unsigned long long>(fnv1a(framed)));
  return digest;
}

/**
 * @brief The driver's `program.hpp`: the generated strategy as `Program`, and `program_fills`, which gives the fill
 *   model the run's market-price rule when it has one.
 * @param request The program and the rule.
 * @return The header's text.
 */
inline std::string program_header(const Request& request) {
  const bool backtest = request.kind_ == DriverKind::BACKTEST;
  const bool rule = !request.fill_header_.empty();
  std::string out = std::string("#pragma once\n\n// Written by cmdatl ") + (backtest ? "strategy backtest" : "run") +
    ".\n\n#include \"strategy.hpp\"\n";
  if (rule) {
    out += "#include \"" + request.fill_header_ + "\"\n";
  }
  out += "\n#include <algotradelib/backtest>\n\n#include <optional>\n#include <type_traits>\n\nusing Program = atls::" +
    request.struct_ + ";\n\n";
  if (request.adapter_) {
    // The broker folder the driver is built from, each byte escaped: a run's driver refuses an operator file naming
    // another.
    out += "#define CMDATL_ADAPTER_FOLDER \"";
    for (const unsigned char c : request.adapter_->folder_) {
      char escaped[5];
      std::snprintf(escaped, sizeof(escaped), "\\%03o", c);
      out += escaped;
    }
    out += "\"\n\n";
  }
  out += "// The fill model, with the run's market-price rule when it has one.\n";
  out +=
    "inline algotradelib::backtest::NextBarFillModel program_fills(algotradelib::backtest::NextBarFillModel model) "
    "{\n";
  if (rule) {
    // One line naming the expected signature, instead of the compiler's candidate notes.
    out += "  static_assert(\n    std::is_invocable_r_v<std::optional<algotradelib::Decimal64>, decltype((" +
      request.fill_function_ +
      ")), const algotradelib::backtest::MarketPriceInput&>,\n    \"replay.fill_function: " + request.fill_function_ +
      " must be callable as std::optional<algotradelib::Decimal64>(const algotradelib::backtest::MarketPriceInput&)\"\n"
      "  );\n";
    out += "  return model.with_market_price(\"" + request.fill_function_ + "\", " + request.fill_function_ + ");\n";
  } else {
    out += "  return model;\n";
  }
  out += "}\n";
  return out;
}

/**
 * @brief A path with its links resolved, or as given when it does not exist.
 * @param path The path.
 * @return The path.
 */
inline std::string real_path(const std::string& path) {
  std::error_code ec;
  const auto resolved = std::filesystem::canonical(path, ec);
  return ec ? path : resolved.string();
}

/**
 * @brief A variable's value in a CMake cache.
 * @param cache The cache's text.
 * @param name The variable.
 * @return Its value, or nothing.
 */
inline std::optional<std::string> cache_value(const std::string& cache, const std::string& name) {
  std::size_t start = 0;
  while (start < cache.size()) {
    auto end = cache.find('\n', start);
    if (end == std::string::npos) {
      end = cache.size();
    }
    const auto line = cache.substr(start, end - start);
    if (line.compare(0, name.size() + 1, name + ":") == 0) {
      const auto eq = line.find('=');
      if (eq != std::string::npos) {
        return line.substr(eq + 1);
      }
    }
    start = end + 1;
  }
  return std::nullopt;
}

/**
 * @brief The last lines of a child's stderr, for a failure's message.
 * @param result The child's end.
 * @return The text.
 */
inline std::string tail_of(const ChildResult& result) {
  std::string out;
  for (const auto& line : result.stderr_tail_) {
    out += (out.empty() ? "" : "\n") + line;
  }
  return out.empty() ? ending_of(result, "the build") : out;
}

/**
 * @brief Builds a driver and copies it into a directory, under the shape's lock.
 * @param request The program, atlc and the driver's kind.
 * @param run_dir The directory, which exists: the run's, or a backtest's scratch directory.
 * @param progress Where progress lines go (and the children's stderr lines).
 * @return A failure reply, or nothing when `run_dir/driver` exists.
 */
inline std::optional<Reply> build_driver(
  const Request& request, const std::filesystem::path& run_dir, const std::function<void(const std::string&)>& progress
) {
#if defined(_WIN32)
  (void)request;
  (void)run_dir;
  (void)progress;
  return error_reply(Exit::FAILED, "unavailable.platform", "runs are built on macOS and Linux only in this version");
#else
  const auto root = cache_root();
  if (!root) {
    return error_reply(
      Exit::FAILED, "cache.unavailable", "neither XDG_CACHE_HOME nor HOME names an absolute directory"
    );
  }
  const std::string package = request.package_;
  // The package the driver must be built against, checked before anything is built, copied or run.
  std::error_code ec;
  if (
    package.empty() ||
    !std::filesystem::is_regular_file(std::filesystem::path(package) / "algotradelibConfig.cmake", ec)
  ) {
    return error_reply(
      Exit::FAILED, "build.failed",
      "the algotradelib package cmdatl was built against is not there: " +
        (package.empty() ? std::string("(none)") : package),
      "reinstall that algotradelib, or rebuild cmdatl against the one installed"
    );
  }
  const bool backtest = request.kind_ == DriverKind::BACKTEST;
  // A store backtest's CMake project links DuckDB, found where the key says: `build.duckdb`, or `DUCKDB_ROOT`, which
  // then joins the key and the build's environment.
  const bool store = backtest && request.store_;
  const bool duckdb_root = store && request.duckdb_.empty();
  std::vector<std::string> extra;
  if (backtest) {
    extra.push_back("backtest");
  }
  // A broker's driver compiles that broker's client; the mock's does not: each is its own shape.
  if (!request.broker_.empty()) {
    extra.push_back("broker=" + request.broker_);
  }
  // A broker folder's driver: its name, where it was read, and its files' fingerprint, so an edited adapter is another
  // shape.
  if (request.adapter_) {
    extra.push_back(
      "adapter=" + request.adapter_->name_ + ";" + request.adapter_->folder_ + ";" + adapter_digest(*request.adapter_)
    );
  }
  if (store) {
    extra.push_back("store");
    if (duckdb_root) {
      const auto root = variable("DUCKDB_ROOT");
      extra.push_back(root ? "DUCKDB_ROOT=" + *root : "DUCKDB_ROOT-");
    } else {
      extra.push_back("duckdb=" + request.duckdb_);
    }
  }
  // The market-price rule changes `program.hpp`, which make may not see when it is rewritten in the second the last build
  // ended (make 3.81 compares whole seconds): each rule is its own shape.
  // The header's bytes too: an edited rule is another shape, so an old build can never run in its place.
  if (!request.fill_header_.empty()) {
    std::string bytes;
    read_file(request.fill_header_, bytes);
    char digest[17];
    std::snprintf(digest, sizeof(digest), "%016llx", static_cast<unsigned long long>(fnv1a(bytes)));
    extra.push_back("fill=" + request.fill_header_ + ";" + request.fill_function_ + ";" + digest);
  }
  const auto name = shape_name(
    request.program_, request.struct_, request.indicators_,
    [](const std::string& key) {
      return variable(key.c_str());
    },
    package, extra
  );
  const std::filesystem::path shape = std::filesystem::path(*root) / name;
  if (make_private_dirs(shape) != 0) {
    return error_reply(Exit::FAILED, "cache.unavailable", "the build cache " + shape.string() + " cannot be made");
  }
  ShapeLock lock;
  if (!lock.take(shape / "lock", progress)) {
    if (interrupted()) {
      return interrupted_reply();
    }
    return error_reply(
      Exit::FAILED, "cache.unavailable", "the build cache's lock " + (shape / "lock").string() + " cannot be taken"
    );
  }
  const auto environment = child_environment(
    own_environment(), duckdb_root ? std::vector<std::string>{"DUCKDB_ROOT"} : std::vector<std::string>{}
  );
  const auto src = shape / "src";
  const auto next = shape / "next";
  remove_stale_temporaries(src);
  std::filesystem::remove_all(next, ec);
  if (make_private_dirs(next) != 0) {
    return error_reply(Exit::FAILED, "cache.unavailable", "the build cache " + next.string() + " cannot be made");
  }
  progress(
    std::string("cmdatl: building the ") + (backtest ? "backtest's" : "run's") + " driver for " + request.struct_ +
    " in " + shape.string()
  );
  const auto generated = run_child(
    {request.atlc_, "build", request.program_, "-o", (next / "strategy.hpp").string(), "--indicators",
      request.indicators_, "--emit-runtime", next.string()},
    progress, &environment
  );
  if (generated.kind_ == ChildResult::Kind::INTERRUPTED) {
    return interrupted_reply();
  }
  if (generated.kind_ != ChildResult::Kind::FINISHED || generated.signaled_ || generated.code_ != 0) {
    if (generated.kind_ == ChildResult::Kind::FINISHED && !generated.signaled_ && generated.code_ == 1) {
      return atlc_failure(
        Exit::PROGRAM, "program.invalid", stderr_text(generated, "the program has errors"), generated, "fix the program"
      );
    }
    return atlc_failure(
      Exit::FAILED, "atlc.failed", "atlc build: " + ending_of(generated), generated, "check atlc with cmdatl env"
    );
  }
  // The runtime the generated strategy includes: an atlc that emits none fails here, not in the compiler.
  if (!std::filesystem::is_regular_file(next / "atls" / "ops.hpp", ec)) {
    return error_reply(
      Exit::FAILED, "atlc.failed",
      "atlc " + request.atlc_version_ + " wrote no atls/ops.hpp, the runtime the generated strategy includes",
      "install a newer atlc"
    );
  }
  // The inputs, each written only when its bytes changed.
  std::map<std::string, std::string> inputs;
  for (const auto& relative : files_under(next)) {
    std::string text;
    if (!read_file(next / relative, text)) {
      return error_reply(
        Exit::FAILED, "build.failed", "atlc's output " + (next / relative).string() + " cannot be read"
      );
    }
    inputs[relative] = std::move(text);
  }
  // A run's driver takes every carried file but the backtest's own; a backtest's driver takes only those, at the root,
  // the replay module and the credential reader.
  const std::string backtest_prefix = "backtest/";
  const std::string replay_prefix = "cmdatl/replay/";
  for (const auto& file : DRIVER_FILES) {
    std::string path = file.path_;
    const bool backtest_file = path.compare(0, backtest_prefix.size(), backtest_prefix) == 0;
    // The descriptor-3 reader too, for a backtest that fetches through a broker folder's adapter.
    const bool replay_file =
      path.compare(0, replay_prefix.size(), replay_prefix) == 0 || path == "cmdatl/system/credential_pipe.hpp";
    if (backtest ? !backtest_file && !replay_file : backtest_file) {
      continue;
    }
    std::string text;
    for (std::size_t k = 0; k < file.count_; ++k) {
      text.append(file.chunks_[k], file.sizes_[k]);
    }
    inputs[backtest_file ? path.substr(backtest_prefix.size()) : path] = std::move(text);
  }
  inputs["program.hpp"] = program_header(request);
  // The broker folder's files as read once: the bytes the checks saw are the bytes compiled.
  if (request.adapter_) {
    for (const auto& [relative, bytes] : request.adapter_->files_) {
      inputs["adapter/" + relative] = bytes;
    }
  }
  for (const auto& [relative, text] : inputs) {
    if (write_if_changed(src / relative, text) == Written::FAILED) {
      return error_reply(
        Exit::FAILED, "build.failed", "the driver's source " + (src / relative).string() + " cannot be written"
      );
    }
  }
  for (const auto& relative : files_under(src)) {
    if (inputs.count(relative) == 0) {
      std::filesystem::remove(src / relative, ec);
    }
  }
  const auto cmake = find_on_path("cmake");
  if (!cmake) {
    return error_reply(
      Exit::FAILED, "build.failed",
      std::string("cmake was not found on PATH; ") + (backtest ? "a backtest" : "a run") +
        " needs CMake and a C++17 compiler"
    );
  }
  const auto build_dir = shape / "build";
  // A cache configured for another package, or before the package was named this way, is configured again.
  std::string cache;
  if (read_file(build_dir / "CMakeCache.txt", cache) && cache_value(cache, "CMDATL_ALGOTRADELIB_DIR") != package) {
    std::filesystem::remove_all(build_dir, ec);
  }
  if (!std::filesystem::exists(build_dir / "CMakeCache.txt", ec)) {
    std::vector<std::string> configure{
      *cmake,
      "-S",
      src.string(),
      "-B",
      build_dir.string(),
      "-DCMAKE_BUILD_TYPE=Release",
      "-DCMDATL_ALGOTRADELIB_DIR=" + package,
    };
    if (!request.broker_.empty()) {
      configure.push_back("-DCMDATL_DRIVER_BROKER=" + request.broker_);
    }
    if (request.adapter_) {
      configure.push_back("-DCMDATL_DRIVER_ADAPTER=" + request.adapter_->name_);
    }
    if (store) {
      configure.push_back("-DCMDATL_STORE=ON");
      if (!duckdb_root) {
        configure.push_back("-DCMDATL_DUCKDB_DIR=" + request.duckdb_);
      }
    }
    const auto configured = run_child(configure, progress, &environment);
    if (configured.kind_ == ChildResult::Kind::INTERRUPTED) {
      return interrupted_reply();
    }
    if (configured.kind_ != ChildResult::Kind::FINISHED || configured.signaled_ || configured.code_ != 0) {
      std::filesystem::remove(build_dir / "CMakeCache.txt", ec);
      return atlc_failure(
        Exit::FAILED, "build.failed", "configuring the driver failed: " + tail_of(configured), configured,
        "check CMake, the compiler" + std::string(store ? ", DuckDB" : "") + " and the algotradelib package " + package
      );
    }
  }
  const auto built = run_child({*cmake, "--build", build_dir.string()}, progress, &environment);
  if (built.kind_ == ChildResult::Kind::INTERRUPTED) {
    return interrupted_reply();
  }
  if (built.kind_ != ChildResult::Kind::FINISHED || built.signaled_ || built.code_ != 0) {
    return atlc_failure(
      Exit::FAILED, "build.failed", "building the driver failed: " + tail_of(built), built,
      "check the compiler and the algotradelib package " + package
    );
  }
  // The package the build used, as CMake recorded it (also after CMake configured again by itself).
  cache.clear();
  const auto used =
    read_file(build_dir / "CMakeCache.txt", cache) ? cache_value(cache, "algotradelib_DIR") : std::nullopt;
  if (!used || real_path(*used) != real_path(package)) {
    std::filesystem::remove_all(build_dir, ec);
    return error_reply(
      Exit::FAILED, "build.failed",
      "the driver was configured against " + used.value_or(std::string("no algotradelib")) + ", not " + package,
      std::string("run cmdatl ") + (backtest ? "strategy backtest" : "run") +
        " again; the driver's build cache was cleared"
    );
  }
  if (const int error = copy_driver(build_dir / "driver", run_dir)) {
    return error_reply(
      Exit::FAILED, "build.failed",
      "the driver could not be copied out of the build cache: " + std::string(std::strerror(error))
    );
  }
  return std::nullopt;
#endif
}

} // namespace build
} // namespace cmdatl
