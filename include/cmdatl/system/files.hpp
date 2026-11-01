#pragma once

// Reading and writing the operator's file, the bars and the account's state files through descriptors.

#include "./syscalls.hpp"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cmdatl {

/** @brief How reading a whole file through its descriptor ended. */
struct FileRead {
  /** @brief What happened. */
  enum class Status {
    OK,
    NOT_REGULAR,
    TOO_LARGE,
    FAILED,
    SHORT,
    CHANGED,
  };
  /** @brief What happened. */
  Status status_{Status::OK};
  /** @brief The error number, for `FAILED`. */
  int error_{0};

  /**
   * @brief The outcome for a person.
   * @param limit The size limit that applied.
   * @return The text, such as `it is not a regular file`.
   */
  std::string text(std::size_t limit) const {
    switch (status_) {
      case Status::OK:
        return "read";
      case Status::NOT_REGULAR:
        return "it is not a regular file";
      case Status::TOO_LARGE:
        return "it is larger than " + std::to_string(limit) + " bytes";
      case Status::FAILED:
        return std::string("it could not be read: ") + std::strerror(error_);
      case Status::SHORT:
        return "it ended before its size was read";
      case Status::CHANGED:
        return "its size changed while it was read";
    }
    return "unknown";
  }
};

#if !defined(_WIN32)
/** @brief A file descriptor, closed when its owner goes, on every path, an exception's included. */
class ScopedFd {
public:
  /**
   * @brief Takes a descriptor.
   * @param fd The descriptor, or -1 for none.
   */
  explicit ScopedFd(int fd = -1) noexcept : fd_(fd) {}

  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;

  /** @brief Closes the descriptor, if any. */
  ~ScopedFd() {
    reset();
  }

  /**
   * @brief The descriptor.
   * @return It, or -1.
   */
  int get() const noexcept {
    return fd_;
  }

  /** @brief Closes the descriptor now. */
  void reset() noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  /**
   * @brief Closes the descriptor, if any, and takes another.
   * @param fd The descriptor, or -1 for none.
   */
  void reset(int fd) noexcept {
    reset();
    fd_ = fd;
  }

private:
  int fd_;
};

/**
 * @brief `open(2)`, with the descriptor moved above the standard ones: with stdin, stdout or stderr closed, a state
 *   file's descriptor is never taken for an output stream (a reply written to it would land in the file).
 * @param path The file.
 * @param flags The flags; `O_CLOEXEC` is kept.
 * @param mode The mode, when creating.
 * @return The descriptor, or -1 with `errno` set.
 */
inline int open_lifted(const char* path, int flags, mode_t mode = 0) noexcept {
  const int fd = ::open(path, flags, mode);
  if (fd < 0 || fd > STDERR_FILENO) {
    return fd;
  }
  const int lifted = ::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
  const int error = errno;
  ::close(fd);
  errno = error;
  return lifted;
}

/**
 * @brief Reads a regular file whole through an open descriptor: its size from `fstat`, at most `limit` bytes, read to
 *   that size (a read error or an early end is a failure), and its size read again afterwards, which must not have
 *   changed.
 * @param fd The descriptor, at offset 0.
 * @param limit The largest size accepted.
 * @param out Where the bytes go.
 * @return How it ended.
 */
inline FileRead read_whole(int fd, std::size_t limit, std::string& out) {
  FileRead read;
  struct stat before{};
  if (sys::fstat(fd, &before) != 0) {
    read.status_ = FileRead::Status::FAILED;
    read.error_ = errno;
    return read;
  }
  if (!S_ISREG(before.st_mode)) {
    read.status_ = FileRead::Status::NOT_REGULAR;
    return read;
  }
  if (before.st_size < 0 || static_cast<unsigned long long>(before.st_size) > limit) {
    read.status_ = FileRead::Status::TOO_LARGE;
    return read;
  }
  const auto size = static_cast<std::size_t>(before.st_size);
  out.assign(size, '\0');
  std::size_t done = 0;
  while (done < size) {
    const auto n = sys::read(fd, &out[done], size - done);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      read.status_ = FileRead::Status::FAILED;
      read.error_ = errno;
      return read;
    }
    if (n == 0) {
      read.status_ = FileRead::Status::SHORT;
      return read;
    }
    done += static_cast<std::size_t>(n);
  }
  struct stat after{};
  if (sys::fstat(fd, &after) != 0) {
    read.status_ = FileRead::Status::FAILED;
    read.error_ = errno;
    return read;
  }
  if (after.st_size != before.st_size) {
    read.status_ = FileRead::Status::CHANGED;
  }
  return read;
}

/**
 * @brief Writes bytes whole to a descriptor, again after `EINTR` and after a short write; a write of zero bytes is a
 *   failure (`EIO`), so a descriptor that takes nothing never loops.
 * @param fd The descriptor.
 * @param data The bytes.
 * @param size How many.
 * @param written Set to how many were written, whatever the outcome.
 * @return 0, or the error number.
 */
inline int write_whole(int fd, const char* data, std::size_t size, std::size_t& written) noexcept {
  written = 0;
  while (written < size) {
    const auto n = sys::write(fd, data + written, size - written);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return errno;
    }
    if (n == 0) {
      return EIO;
    }
    written += static_cast<std::size_t>(n);
  }
  return 0;
}

/**
 * @brief Whether a descriptor's file is a regular file with one name.
 * @param fd The descriptor.
 * @param error Set to the error number when its status cannot be read.
 * @return Whether it is; false when its status cannot be read.
 */
inline bool single_regular(int fd, int& error) noexcept {
  struct stat info{};
  if (sys::fstat(fd, &info) != 0) {
    error = errno;
    return false;
  }
  error = 0;
  return S_ISREG(info.st_mode) && info.st_nlink == 1;
}

/**
 * @brief Flushes a directory's entries to disk, so a file created in it survives a killed process with its name.
 * @param dir The directory.
 * @return 0, or the error number.
 */
inline int sync_directory(const std::string& dir) noexcept {
  const int fd = open_lifted(dir.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
  if (fd < 0) {
    return errno;
  }
  const int synced = sys::fsync(fd) == 0 ? 0 : errno;
  ::close(fd);
  return synced;
}
#endif

} // namespace cmdatl
