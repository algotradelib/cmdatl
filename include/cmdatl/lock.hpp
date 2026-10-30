#pragma once

// The account's owner lock and its owner record.
//
// What the lock guarantees: two cmdatl processes never hold an account at once, provided nobody removes or replaces
// its `lock` file while it is held (cmdatl never removes it).
// A replacement while a process waits is caught at acquisition; a replacement after acquisition is caught at the
// holder's next check, made before every write to the account and before its reply.
// The window between a check and the write it guards is not closed: whoever can replace `lock` is the agent's own
// user, who can edit every state file anyway.

#include "./files.hpp"

#include <boost/json.hpp>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <functional>
#include <optional>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cmdatl {

/** @brief How long an account command waits for the account, at most. */
inline constexpr std::chrono::milliseconds LOCK_WAIT{10000};
/** @brief How often a waiting command tries the lock again. */
inline constexpr std::chrono::milliseconds LOCK_STEP{20};
/** @brief The largest owner record read, in bytes. */
inline constexpr std::size_t MAX_OWNER_BYTES = 4096;

#if !defined(_WIN32)
/** @brief The file a held lock is on, by device and inode, so a check needs no descriptor of its own. */
struct LockIdentity {
  /** @brief The lock's path. */
  std::string path_;
  /** @brief Its device. */
  dev_t dev_{0};
  /** @brief Its inode. */
  ino_t ino_{0};

  /**
   * @brief Whether the file at the lock's path is still the one that was locked.
   * @return Whether it is; false when the path cannot be read.
   */
  bool still_at_path() const {
    struct stat info{};
    return !path_.empty() && ::stat(path_.c_str(), &info) == 0 && info.st_dev == dev_ && info.st_ino == ino_;
  }
};

/**
 * @brief Reads the owner record beside a lock, without the lock: opened without following a link or blocking, a
 *   regular file of at most 4 KiB, one JSON object.
 * @param dir The account's directory.
 * @return The record, or nothing when it is absent or not readable as one.
 */
inline std::optional<boost::json::object> read_owner_record(const std::string& dir) {
  ScopedFd fd(open_lifted((dir + "/owner.json").c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
  if (fd.get() < 0) {
    return std::nullopt;
  }
  std::string text;
  if (read_whole(fd.get(), MAX_OWNER_BYTES, text).status_ != FileRead::Status::OK) {
    return std::nullopt;
  }
  boost::system::error_code error;
  auto value = boost::json::parse(text, error);
  if (error || !value.is_object()) {
    return std::nullopt;
  }
  return std::move(value.as_object());
}

/**
 * @brief Who holds or last held the account, for a person: `status (pid 123)`.
 * @param dir The account's directory.
 * @return The text, or `another process` when the record says nothing.
 */
inline std::string holder_text(const std::string& dir) {
  const auto record = read_owner_record(dir);
  if (!record) {
    return "another process";
  }
  std::string text;
  if (const auto* what = record->if_contains("what"); what != nullptr && what->is_string()) {
    text = std::string(what->as_string());
  }
  if (const auto* pid = record->if_contains("pid"); pid != nullptr && pid->is_int64()) {
    text += (text.empty() ? "" : " ") + std::string("(pid ") + std::to_string(pid->as_int64()) + ")";
  }
  return text.empty() ? "another process" : text;
}

/**
 * @brief Whether an open descriptor is the file now at a path (same device and inode).
 * @param fd The descriptor.
 * @param path The path.
 * @return False when either cannot be read, or they differ.
 */
inline bool same_file(int fd, const std::string& path) {
  struct stat by_fd{};
  struct stat by_path{};
  return sys::fstat(fd, &by_fd) == 0 && ::stat(path.c_str(), &by_path) == 0 && by_fd.st_dev == by_path.st_dev &&
    by_fd.st_ino == by_path.st_ino;
}

/** @brief The exclusive lock on an account's `lock` file, held for a command's whole duration. */
class OwnerLock {
public:
  /** @brief How an acquisition ended. */
  enum class Outcome {
    HELD,
    BUSY,
    INTERRUPTED,
    FAILED,
  };

  /**
   * @brief Names the lock of an account's directory; nothing is opened yet.
   * @param dir The account's directory.
   */
  explicit OwnerLock(std::string dir)
  : dir_(std::move(dir)), path_(dir_ + "/lock") {}

  OwnerLock(const OwnerLock&) = delete;
  OwnerLock& operator=(const OwnerLock&) = delete;
  OwnerLock(OwnerLock&&) = delete;
  OwnerLock& operator=(OwnerLock&&) = delete;

  /** @brief Releases the lock (closing the descriptor drops the `flock`). */
  ~OwnerLock() = default;

  /**
   * @brief Takes the lock: the file opened without following a link (one name, a regular file), `flock` tried every
   *   20 ms, and the locked descriptor checked to be the file at the path; a file replaced while this process waits,
   *   or before its check, is opened again.
   * @param timeout How long to wait for another holder.
   * @param interrupted Whether SIGINT or SIGTERM arrived; checked on every pass.
   * @param on_wait Called once, after the first refused try, with who holds it.
   * @return How it ended; `error()` says why for `FAILED`.
   */
  Outcome acquire(
    std::chrono::milliseconds timeout,
    const std::function<bool()>& interrupted,
    const std::function<void(const std::string&)>& on_wait
  ) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    bool told = false;
    while (true) {
      fd_.reset(open_lifted(path_.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600));
      if (fd_.get() < 0) {
        error_ = "the account's lock " + path_ + " cannot be opened: " + std::strerror(errno);
        return Outcome::FAILED;
      }
      int status_error = 0;
      if (!single_regular(fd_.get(), status_error)) {
        error_ = status_error != 0 ?
          "the account's lock " + path_ + " cannot be checked: " + std::strerror(status_error) :
          "the account's lock " + path_ + " is not a regular file with one name";
        fd_.reset();
        return Outcome::FAILED;
      }
      bool locked = false;
      while (true) {
        if (::flock(fd_.get(), LOCK_EX | LOCK_NB) == 0) {
          locked = true;
          break;
        }
        if (errno != EWOULDBLOCK && errno != EINTR) {
          error_ = "the account's lock " + path_ + " cannot be taken: " + std::strerror(errno);
          fd_.reset();
          return Outcome::FAILED;
        }
        if (interrupted()) {
          fd_.reset();
          return Outcome::INTERRUPTED;
        }
        if (!told) {
          told = true;
          on_wait(holder_text(dir_));
        }
        if (std::chrono::steady_clock::now() >= until) {
          error_ = holder_text(dir_);
          fd_.reset();
          return Outcome::BUSY;
        }
        std::this_thread::sleep_for(LOCK_STEP);
        if (!same_file(fd_.get(), path_)) {
          // The lock file was replaced while this process waited: wait on the one now at the path.
          break;
        }
      }
      struct stat by_fd{};
      if (locked && sys::fstat(fd_.get(), &by_fd) == 0 && same_file(fd_.get(), path_)) {
        identity_ = LockIdentity{path_, by_fd.st_dev, by_fd.st_ino};
        held_ = true;
        return Outcome::HELD;
      }
      // The lock file was replaced while this process waited: lock the one now at the path.
      fd_.reset();
      if (interrupted()) {
        return Outcome::INTERRUPTED;
      }
      if (std::chrono::steady_clock::now() >= until) {
        error_ = holder_text(dir_);
        return Outcome::BUSY;
      }
    }
  }

  /**
   * @brief Whether this process holds it.
   * @return Whether it does.
   */
  bool held() const noexcept { return held_; }

  /**
   * @brief The account's directory.
   * @return The directory.
   */
  const std::string& dir() const noexcept { return dir_; }

  /**
   * @brief The lock file.
   * @return The path.
   */
  const std::string& path() const noexcept { return path_; }

  /**
   * @brief The locked file's identity, for a check without the descriptor.
   * @return The identity; empty when not held.
   */
  const LockIdentity& identity() const noexcept { return identity_; }

  /**
   * @brief Whether the held descriptor is still the file at the lock's path.
   * @return Whether it is; false when not held.
   */
  bool still_at_path() const { return held_ && identity_.still_at_path(); }

  /**
   * @brief Why it was not taken: the failure, or who holds it.
   * @return The text.
   */
  const std::string& error() const noexcept { return error_; }

private:
  std::string dir_;
  std::string path_;
  ScopedFd fd_;
  LockIdentity identity_;
  bool held_{false};
  std::string error_;
};

/**
 * @brief Writes the owner record after an account opened: to `owner.<pid>.tmp`, created exclusively (never an
 *   existing file, never truncated), then renamed onto `owner.json`; the lock checked before each.
 *
 * The record describes; the lock decides: every other failure is ignored, and a temporary this call created is
 * removed.
 * @param lock The account's lock, held.
 * @param what The command, at most 200 bytes.
 * @return False when the lock was no longer this process's, so nothing more may be written; true otherwise.
 */
inline bool write_owner_record(const OwnerLock& lock, const std::string& what) noexcept {
  try {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
    gmtime_r(&now, &utc);
    char since[32];
    std::strftime(since, sizeof(since), "%Y-%m-%dT%H:%M:%SZ", &utc);
    const boost::json::object body{
      {"role", "agent"},
      {"pid", static_cast<std::int64_t>(::getpid())},
      {"what", what.substr(0, 200)},
      {"since", since},
    };
    const auto text = boost::json::serialize(body) + "\n";
    const auto temporary = lock.dir() + "/owner." + std::to_string(::getpid()) + ".tmp";
    const auto record = lock.dir() + "/owner.json";
    if (!lock.still_at_path()) {
      return false;
    }
    ScopedFd fd(open_lifted(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (fd.get() < 0) {
      return true;
    }
    std::size_t written = 0;
    const bool ok = write_whole(fd.get(), text.data(), text.size(), written) == 0;
    fd.reset();
    if (!ok) {
      ::unlink(temporary.c_str());
      return true;
    }
    if (!lock.still_at_path()) {
      ::unlink(temporary.c_str());
      return false;
    }
    if (sys::rename(temporary.c_str(), record.c_str()) != 0) {
      ::unlink(temporary.c_str());
    }
  } catch (...) {
    // A description only: an allocation failing here leaves the last holder's record.
  }
  return true;
}
#endif

}  // namespace cmdatl
