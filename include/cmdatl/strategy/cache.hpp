#pragma once

// The build cache's commands: where it is (`cache dir`), how large each of its folders is (`cache size`), and clearing
// the builds no process is using (`cache clean`).
//
// A clean touches only what looks like a shape `build_driver` makes under `drivers/`: a real directory named as
// `shape_name` names one, holding its `lock`. Under that lock, taken without waiting, it removes the shape's `src/`,
// `next/` and `build/`, which the next build of the shape makes again; the shape's folder and its lock stay, so a build
// waiting on the lock gets the same file when the clean lets go. Every step goes through descriptors opened without
// following a link, from the validated `drivers/` down, and never enters a directory on another file system than
// `drivers/`'s. A name and a lock show the layout, not who made it: `drivers/` is reserved for cmdatl's builds, as the
// docs say.

#include "../core/reply.hpp"
#include "../system/files.hpp"
#include "./build.hpp"
#include "./spec.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// openat2(2), Linux 5.6 and later: its RESOLVE_NO_XDEV refuses to cross any mount, a bind mount of the same file
// system included, which a device number cannot show.
#if defined(__linux__) && defined(__has_include)
#if __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#include <sys/syscall.h>
#if defined(SYS_openat2)
#define CMDATL_HAS_OPENAT2 1
#endif
#endif
#endif

namespace cmdatl {
namespace cache {

/** @brief The folders of cmdatl's cache that `cache size` measures. */
inline constexpr const char* BUCKETS[] = {"drivers", "backtests", "calendars", "tokens"};

/** @brief The folders inside a shape that a clean removes: what the next build makes again. */
inline constexpr const char* CLEARED_FOLDERS[] = {"src", "next", "build"};

/** @brief How deep a removal or a measurement goes before it stops with an error. */
inline constexpr int MAX_DEPTH = 128;

/** @brief What `cache.partial` tells its caller to do. */
inline constexpr const char* PARTIAL_NEXT = "read failed[].error; a permission error needs the operator";

/**
 * @brief cmdatl's cache folder, `<base>/cmdatl`, as the build cache's root names it.
 * @return The folder, or nothing when neither `XDG_CACHE_HOME` nor `HOME` is absolute.
 */
inline std::optional<std::string> cache_dir() {
  const auto root = build::cache_root();
  if (!root) {
    return std::nullopt;
  }
  return std::filesystem::path(*root).parent_path().string();
}

/**
 * @brief The refusal when the cache's folder cannot be named.
 * @return The reply.
 */
inline Reply unresolved_reply() {
  return error_reply(Exit::FAILED, "cache.unavailable", "neither XDG_CACHE_HOME nor HOME names an absolute directory");
}

/**
 * @brief The refusal on a platform the cache commands do not run on.
 * @return The reply.
 */
inline Reply platform_reply() {
  return error_reply(Exit::FAILED, "unavailable.platform", "the cache commands run on macOS and Linux only");
}

/**
 * @brief Whether a name is one `shape_name` makes: a program's struct, `-`, then 16 lowercase hex digits.
 * @param name The name.
 * @return Whether it is.
 */
inline bool shape_named(const std::string& name) {
  constexpr std::size_t DIGITS = 16;
  if (name.size() < DIGITS + 2 || name[name.size() - DIGITS - 1] != '-') {
    return false;
  }
  const auto digits = name.substr(name.size() - DIGITS);
  const bool hex = std::all_of(digits.begin(), digits.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
  return hex && spec::valid_struct(name.substr(0, name.size() - DIGITS - 1));
}

/**
 * @brief `cmdatl cache dir`: where cmdatl's cache is.
 * @return The reply: `path`.
 */
inline Reply dir_reply() {
#if defined(_WIN32)
  return platform_reply();
#else
  const auto dir = cache_dir();
  if (!dir) {
    return unresolved_reply();
  }
  Reply reply;
  reply.body_["path"] = *dir;
  return reply;
#endif
}

#if !defined(_WIN32)
/** @brief Closes a directory stream, its descriptor with it. */
struct DirCloser {
  /**
   * @brief Closes it.
   * @param stream The stream.
   */
  void operator()(DIR* stream) const noexcept {
    ::closedir(stream);
  }
};

/** @brief Why a walk stopped at a directory nested too deep. */
inline constexpr const char* TOO_DEEP = "more than 128 folders deep; not entered";
static_assert(MAX_DEPTH == 128, "TOO_DEEP names MAX_DEPTH");

/** @brief Why a clean stopped at a directory on another file system than `drivers/`. */
inline constexpr const char* OTHER_FILE_SYSTEM = "on another file system (a mount); not entered";

/** @brief Why a clean stopped at a mount point openat2 refused to cross. */
inline constexpr const char* MOUNT_POINT = "crosses a mount point; not entered";

/** @brief Why a clean on Linux refuses to start without openat2. */
inline constexpr const char* NO_OPENAT2 =
  "this system cannot check mount points (openat2, Linux 5.6 or later, is unavailable)";

/**
 * @brief The names in a directory, but `.` and `..`, sorted.
 * @param dir The directory's descriptor, which stays open.
 * @param names Set to the names.
 * @return 0, or the error number.
 */
inline int names_in(int dir, std::vector<std::string>& names) {
  names.clear();
  const int copy = ::fcntl(dir, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
  if (copy < 0) {
    return errno;
  }
  const std::unique_ptr<DIR, DirCloser> stream(::fdopendir(copy));
  if (!stream) {
    const int error = errno;
    ::close(copy);
    return error;
  }
  int error = 0;
  for (;;) {
    errno = 0;
    const dirent* entry = ::readdir(stream.get());
    if (entry == nullptr) {
      error = errno;
      break;
    }
    const std::string name = entry->d_name;
    if (name != "." && name != "..") {
      names.push_back(name);
    }
  }
  std::sort(names.begin(), names.end());
  return error;
}

/**
 * @brief Opens a directory inside another, never through a link.
 * @param parent The parent's descriptor.
 * @param name The directory's name.
 * @return The descriptor, or -1 with `errno` set (`ELOOP` or `ENOTDIR` for a link or another kind of file).
 */
inline int open_directory_at(int parent, const std::string& name) noexcept {
  return openat_lifted(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

/** @brief How a clean opens a directory below `drivers/`. */
inline constexpr int DIRECTORY_FLAGS = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

/** @brief How a clean opens a shape's lock: never created, never through a link, never blocking on a FIFO. */
inline constexpr int LOCK_FLAGS = O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;

/**
 * @brief Opens a file or a directory inside a directory for a clean, never through a link nor across a mount: on Linux
 *   only with openat2's `RESOLVE_NO_XDEV`, which sees a bind mount of the same file system (without openat2 it opens
 *   nothing: `ENOSYS`); elsewhere with `openat`, and the caller compares devices.
 * @param parent The parent's descriptor.
 * @param name The name.
 * @param flags The open flags: `DIRECTORY_FLAGS` or `LOCK_FLAGS`.
 * @return The descriptor, or -1 with `errno` set (`EXDEV` for a mount point, `ENOSYS` without openat2).
 */
inline int open_inside(int parent, const std::string& name, int flags) noexcept {
#if defined(CMDATL_HAS_OPENAT2)
  open_how how{};
  how.flags = static_cast<std::uint64_t>(flags);
  how.resolve = RESOLVE_NO_XDEV | RESOLVE_NO_SYMLINKS;
  const int fd = static_cast<int>(::syscall(SYS_openat2, parent, name.c_str(), &how, sizeof how));
  if (fd < 0 || fd > STDERR_FILENO) {
    return fd;
  }
  const int lifted = ::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
  const int error = errno;
  ::close(fd);
  errno = error;
  return lifted;
#elif defined(__linux__)
  (void)parent;
  (void)name;
  (void)flags;
  errno = ENOSYS;
  return -1;
#else
  return openat_lifted(parent, name.c_str(), flags);
#endif
}

/**
 * @brief Why a clean could not open a directory.
 * @param error The error number.
 * @return The text: `MOUNT_POINT` for a mount point, `NO_OPENAT2` without openat2.
 */
inline std::string open_failure(int error) {
  if (error == EXDEV) {
    return MOUNT_POINT;
  }
  return error == ENOSYS ? std::string(NO_OPENAT2) + "; not entered" : std::string(std::strerror(error));
}

/**
 * @brief Removes a directory and everything in it, relative to its parent's descriptor: a link inside is removed as a
 *   link, never followed, and a directory on another file system than `drivers/` is never entered; it goes on past a
 *   failure and removes what it can.
 * @param parent The parent's descriptor.
 * @param name The directory's name.
 * @param path The directory's path in the shape, for a failure's text.
 * @param depth How deep this directory is.
 * @param device The file system of `drivers/`.
 * @param failure Set to the first failure, `<path>: <reason>`, when it is empty.
 * @return Whether the directory was removed whole.
 */
inline bool remove_tree(
  int parent, const std::string& name, const std::string& path, int depth, dev_t device, std::string& failure
) {
  const auto fail = [&failure](const std::string& at, const std::string& why) -> bool {
    if (failure.empty()) {
      failure = at + ": " + why;
    }
    return false;
  };
  if (depth >= MAX_DEPTH) {
    return fail(path, TOO_DEEP);
  }
  const ScopedFd dir(open_inside(parent, name, DIRECTORY_FLAGS));
  if (dir.get() < 0) {
    const int error = errno;
    return fail(path, open_failure(error));
  }
  struct stat info{};
  if (::fstat(dir.get(), &info) != 0) {
    const int error = errno;
    return fail(path, std::strerror(error));
  }
  if (info.st_dev != device) {
    return fail(path, OTHER_FILE_SYSTEM);
  }
  std::vector<std::string> names;
  bool whole = true;
  if (const int error = names_in(dir.get(), names)) {
    whole = fail(path, std::strerror(error));
  }
  for (const auto& entry : names) {
    const auto at = path + "/" + entry;
    // Anything but a directory, a link included, is unlinked; one whose type cannot be read is tried as a file.
    if (::fstatat(dir.get(), entry.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(info.st_mode)) {
      whole = remove_tree(dir.get(), entry, at, depth + 1, device, failure) && whole;
    } else if (::unlinkat(dir.get(), entry.c_str(), 0) != 0) {
      const int error = errno;
      whole = fail(at, std::strerror(error));
    }
  }
  if (whole && ::unlinkat(parent, name.c_str(), AT_REMOVEDIR) != 0) {
    const int error = errno;
    whole = fail(path, std::strerror(error));
  }
  return whole;
}

/** @brief A folder's size as `cache size` reports it. */
struct Measured {
  /** @brief The sum of its regular files' sizes, links never followed. */
  std::uint64_t bytes_{0};
  /** @brief Its entries, at the top level. */
  std::uint64_t entries_{0};
  /** @brief The first failure, with the path it happened at; empty when every part was read. */
  std::string error_;
};

/**
 * @brief Adds a directory's regular files' sizes to a measurement, links never followed.
 * @param dir The directory's descriptor.
 * @param path The directory's path, for a failure's text.
 * @param depth How deep this directory is.
 * @param measured Where the sizes and the first failure go.
 */
inline void measure_tree(int dir, const std::string& path, int depth, Measured& measured) {
  const auto fail = [&measured](const std::string& at, const std::string& why) {
    if (measured.error_.empty()) {
      measured.error_ = at + ": " + why;
    }
  };
  std::vector<std::string> names;
  if (const int error = names_in(dir, names)) {
    fail(path, std::strerror(error));
  }
  if (depth == 0) {
    measured.entries_ = names.size();
  }
  for (const auto& name : names) {
    const auto at = path + "/" + name;
    struct stat info{};
    if (::fstatat(dir, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
      fail(at, std::strerror(errno));
    } else if (S_ISREG(info.st_mode)) {
      measured.bytes_ += static_cast<std::uint64_t>(info.st_size);
    } else if (S_ISDIR(info.st_mode)) {
      if (depth + 1 >= MAX_DEPTH) {
        fail(at, TOO_DEEP);
        continue;
      }
      const ScopedFd inner(open_directory_at(dir, name));
      if (inner.get() < 0) {
        fail(at, std::strerror(errno));
      } else {
        measure_tree(inner.get(), at, depth + 1, measured);
      }
    }
  }
}
#endif

/**
 * @brief `cmdatl cache size`: each folder's bytes and entries, links never followed; it reads only.
 * @return The reply: `path`, an object per folder, and `bytes` in total; `cache.unavailable` with the same fields when
 *   a part could not be read, so a partial count never reads as a success.
 */
inline Reply size_reply() {
#if defined(_WIN32)
  return platform_reply();
#else
  const auto dir = cache_dir();
  if (!dir) {
    return unresolved_reply();
  }
  Reply reply;
  reply.body_["path"] = *dir;
  const ScopedFd root(open_lifted(dir->c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  const int root_error = root.get() < 0 ? errno : 0;
  std::uint64_t total = 0;
  std::string first_error;
  for (const std::string bucket : BUCKETS) {
    const auto path = *dir + "/" + bucket;
    Measured measured;
    if (root_error != 0) {
      if (root_error != ENOENT) {
        measured.error_ = *dir + ": " + std::strerror(root_error);
      }
    } else {
      const ScopedFd folder(open_directory_at(root.get(), bucket));
      if (folder.get() >= 0) {
        measure_tree(folder.get(), path, 0, measured);
      } else if (errno != ENOENT) {
        measured.error_ = path + ": " + std::strerror(errno);
      }
    }
    json::object out;
    out["bytes"] = measured.bytes_;
    out["entries"] = measured.entries_;
    if (!measured.error_.empty()) {
      out["error"] = measured.error_;
      if (first_error.empty()) {
        first_error = measured.error_;
      }
    }
    reply.body_[bucket] = std::move(out);
    total += measured.bytes_;
  }
  reply.body_["bytes"] = total;
  if (first_error.empty()) {
    return reply;
  }
  auto failed = error_reply(
    Exit::FAILED, "cache.unavailable", "the cache could not be read whole: " + first_error,
    "the operator makes that folder readable; the counts in this reply leave out what could not be read"
  );
  for (const auto& field : reply.body_) {
    failed.body_[field.key()] = field.value();
  }
  return failed;
#endif
}

#if !defined(_WIN32)
/**
 * @brief Opens a folder of the cache for a clean: a real directory, not a link, owned by this user.
 * @param parent The parent's descriptor, or `AT_FDCWD` for an absolute path.
 * @param name The folder's name or path.
 * @param fd Set to its descriptor.
 * @param why Set to the reason it was refused.
 * @return 0, `ENOENT` when it does not exist, or another error number.
 */
inline int open_owned(int parent, const std::string& name, ScopedFd& fd, std::string& why) {
  fd.reset(open_directory_at(parent, name));
  if (fd.get() < 0) {
    const int error = errno;
    why = error == ELOOP || error == ENOTDIR ? "it is not a directory (or is a symbolic link)" : std::strerror(error);
    return error;
  }
  struct stat info{};
  if (::fstat(fd.get(), &info) != 0) {
    const int error = errno;
    why = std::strerror(error);
    fd.reset();
    return error;
  }
  if (!S_ISDIR(info.st_mode) || info.st_uid != ::getuid()) {
    why = "it is not a directory owned by this user";
    fd.reset();
    return EPERM;
  }
  return 0;
}

/** @brief What one shape's clean came to. */
enum class ShapeClean {
  /** @brief Its contents were removed. */
  CLEARED,
  /** @brief It held nothing to remove. */
  EMPTY,
  /** @brief Another process holds its lock. */
  IN_USE,
  /** @brief It is not a shape cmdatl made: by its type, or a missing lock. */
  IGNORED,
  /** @brief Something could not be removed or read. */
  FAILED,
};

/**
 * @brief Clears one entry of `drivers/` if it is a shape: its `src/`, `next/` and `build/`, under its lock.
 * @param drivers The descriptor of `drivers/`.
 * @param device The file system of `drivers/`: a shape or a folder in it on another is not entered.
 * @param name The entry's name.
 * @param error Set to the failure, for `FAILED`.
 * @return What happened.
 */
inline ShapeClean clean_shape(int drivers, dev_t device, const std::string& name, std::string& error) {
  if (!shape_named(name)) {
    return ShapeClean::IGNORED;
  }
  struct stat info{};
  if (::fstatat(drivers, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno == ENOENT) {
      return ShapeClean::IGNORED;
    }
    error = std::strerror(errno);
    return ShapeClean::FAILED;
  }
  if (!S_ISDIR(info.st_mode)) {
    return ShapeClean::IGNORED;
  }
  if (info.st_dev != device) {
    error = OTHER_FILE_SYSTEM;
    return ShapeClean::FAILED;
  }
  const ScopedFd shape(open_inside(drivers, name, DIRECTORY_FLAGS));
  if (shape.get() < 0) {
    const int failed = errno;
    if (failed == ENOENT || failed == ELOOP || failed == ENOTDIR) {
      return ShapeClean::IGNORED;
    }
    error = open_failure(failed);
    return ShapeClean::FAILED;
  }
  // The directory opened, not the name checked above: a mount made in between is caught before its lock is touched.
  if (::fstat(shape.get(), &info) != 0) {
    error = std::strerror(errno);
    return ShapeClean::FAILED;
  }
  if (!S_ISDIR(info.st_mode) || info.st_dev != device) {
    error = OTHER_FILE_SYSTEM;
    return ShapeClean::FAILED;
  }
  // The lock as build_driver makes it, never created here: a regular file, not reached through a link nor a mount, so
  // the clean locks the file a builder locks.
  const ScopedFd lock(open_inside(shape.get(), "lock", LOCK_FLAGS));
  if (lock.get() < 0) {
    const int failed = errno;
    if (failed == ENOENT || failed == ELOOP || failed == EISDIR || failed == ENXIO) {
      return ShapeClean::IGNORED;
    }
    error = "lock: " + open_failure(failed);
    return ShapeClean::FAILED;
  }
  if (::fstat(lock.get(), &info) != 0) {
    error = std::string("lock: ") + std::strerror(errno);
    return ShapeClean::FAILED;
  }
  if (info.st_dev != device) {
    error = std::string("lock: ") + MOUNT_POINT;
    return ShapeClean::FAILED;
  }
  if (!S_ISREG(info.st_mode)) {
    return ShapeClean::IGNORED;
  }
  int locked = 0;
  do {
    locked = ::flock(lock.get(), LOCK_EX | LOCK_NB);
  } while (locked != 0 && errno == EINTR);
  if (locked != 0) {
    if (errno == EWOULDBLOCK) {
      return ShapeClean::IN_USE;
    }
    error = std::string("lock: ") + std::strerror(errno);
    return ShapeClean::FAILED;
  }
  // Under the lock: only the three folders, each only when it is a real directory. The lock goes when `lock` closes.
  bool removed = false;
  for (const std::string folder : CLEARED_FOLDERS) {
    if (::fstatat(shape.get(), folder.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno != ENOENT && error.empty()) {
        error = folder + ": " + std::strerror(errno);
      }
      continue;
    }
    if (!S_ISDIR(info.st_mode)) {
      continue;
    }
    if (remove_tree(shape.get(), folder, folder, 0, device, error)) {
      removed = true;
    }
  }
  if (!error.empty()) {
    return ShapeClean::FAILED;
  }
  return removed ? ShapeClean::CLEARED : ShapeClean::EMPTY;
}
#endif

/**
 * @brief The reply's counts: what a clean came to.
 * @param reply The reply the fields go in.
 * @param cleared The shapes cleared.
 * @param in_use The shapes another process holds.
 * @param ignored The entries left alone.
 * @param failed The failures.
 */
inline void put_counts(
  Reply& reply, std::uint64_t cleared, std::uint64_t in_use, std::uint64_t ignored, json::array failed
) {
  reply.body_["cleared"] = cleared;
  reply.body_["in_use"] = in_use;
  reply.body_["ignored"] = ignored;
  reply.body_["failed"] = std::move(failed);
}

/**
 * @brief `cmdatl cache clean`: clears every build no process is using, under its lock; the shape's folder and lock
 *   stay, and nothing but `drivers/` is touched.
 * @return The reply: `cleared`, `in_use`, `ignored`, `failed`; `cache.partial` with the same fields when something
 *   could not be cleared.
 */
inline Reply clean_reply() {
#if defined(_WIN32)
  return platform_reply();
#else
  const auto dir = cache_dir();
  if (!dir) {
    return unresolved_reply();
  }
  Reply done;
  put_counts(done, 0, 0, 0, json::array{});
  const auto refuse = [](const std::string& path, const std::string& why) {
    return error_reply(
      Exit::FAILED, "cache.unavailable", path + " cannot be cleaned: " + why + "; nothing was cleared",
      "the operator checks " + path
    );
  };
  ScopedFd root;
  std::string why;
  if (const int error = open_owned(AT_FDCWD, *dir, root, why)) {
    return error == ENOENT ? done : refuse(*dir, why);
  }
  const auto drivers_path = *dir + "/drivers";
  ScopedFd drivers;
  if (const int error = open_owned(root.get(), "drivers", drivers, why)) {
    return error == ENOENT ? done : refuse(drivers_path, why);
  }
  struct stat bucket{};
  if (::fstat(drivers.get(), &bucket) != 0) {
    return refuse(drivers_path, std::strerror(errno));
  }
  // On Linux every folder below drivers/ is opened with openat2, so it is tried once first: without it nothing is
  // touched, as a bind mount would go unseen.
#if defined(__linux__)
  {
    const ScopedFd probe(open_inside(drivers.get(), ".", DIRECTORY_FLAGS));
    if (probe.get() < 0) {
      const int error = errno;
      auto reply = refuse(drivers_path, error == ENOSYS ? NO_OPENAT2 : "openat2: " + std::string(std::strerror(error)));
      put_counts(reply, 0, 0, 0, json::array{});
      return reply;
    }
  }
#endif
  build::write_cache_tag(drivers.get());
  std::vector<std::string> names;
  if (const int error = names_in(drivers.get(), names)) {
    return refuse(drivers_path, std::strerror(error));
  }
  std::uint64_t cleared = 0;
  std::uint64_t in_use = 0;
  std::uint64_t ignored = 0;
  json::array failed;
  for (const auto& name : names) {
    if (name == build::CACHE_TAG_NAME) {
      continue;
    }
    std::string error;
    switch (clean_shape(drivers.get(), bucket.st_dev, name, error)) {
      case ShapeClean::CLEARED:
        ++cleared;
        break;
      case ShapeClean::EMPTY:
        break;
      case ShapeClean::IN_USE:
        ++in_use;
        break;
      case ShapeClean::IGNORED:
        ++ignored;
        break;
      case ShapeClean::FAILED:
        failed.push_back(json::object{{"path", drivers_path + "/" + name}, {"error", error}});
        break;
    }
  }
  if (failed.empty()) {
    put_counts(done, cleared, in_use, ignored, std::move(failed));
    return done;
  }
  const auto count = failed.size();
  auto reply = error_reply(
    Exit::FAILED, "cache.partial",
    std::to_string(count) + (count == 1 ? " build" : " builds") + " could not be cleared", PARTIAL_NEXT
  );
  put_counts(reply, cleared, in_use, ignored, std::move(failed));
  return reply;
#endif
}

} // namespace cache
} // namespace cmdatl
