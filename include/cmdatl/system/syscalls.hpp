#pragma once

// The system calls the account's state files go through, and the points a test may kill the process at.
//
// In cmdatl these are the real calls, inline.
// Only the separate fault-test executable defines CMDATL_FAULT_HOOKS, in every one of its translation units, and gives
// its own definitions: so no translation unit sees both an inline definition and the test's, and no shipping binary
// carries a fault switch.

#if !defined(_WIN32)
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cstdio>

namespace cmdatl {
namespace sys {

#if defined(CMDATL_FAULT_HOOKS)
/**
 * @brief `read(2)`, or the fault test's stand-in.
 * @param fd The descriptor.
 * @param buffer Where the bytes go.
 * @param size How many at most.
 * @return As `read`.
 */
ssize_t read(int fd, void* buffer, std::size_t size);
/**
 * @brief `write(2)`, or the fault test's stand-in.
 * @param fd The descriptor.
 * @param buffer The bytes.
 * @param size How many.
 * @return As `write`.
 */
ssize_t write(int fd, const void* buffer, std::size_t size);
/**
 * @brief `fsync(2)`, or the fault test's stand-in.
 * @param fd The descriptor.
 * @return As `fsync`.
 */
int fsync(int fd);
/**
 * @brief `ftruncate(2)`, or the fault test's stand-in.
 * @param fd The descriptor.
 * @param size The new size.
 * @return As `ftruncate`.
 */
int ftruncate(int fd, off_t size);
/**
 * @brief `rename(2)`, or the fault test's stand-in.
 * @param from The old name.
 * @param to The new name.
 * @return As `rename`.
 */
int rename(const char* from, const char* to);
/**
 * @brief `fstat(2)`, or the fault test's stand-in.
 * @param fd The descriptor.
 * @param info Where the status goes.
 * @return As `fstat`.
 */
int fstat(int fd, struct stat* info);
/**
 * @brief A point the fault test may kill the process at; nothing in cmdatl.
 * @param name The point, such as `advance.synced`.
 */
void kill_point(const char* name);
#else
/**
 * @brief `read(2)`.
 * @param fd The descriptor.
 * @param buffer Where the bytes go.
 * @param size How many at most.
 * @return As `read`.
 */
inline ssize_t read(int fd, void* buffer, std::size_t size) {
  return ::read(fd, buffer, size);
}

/**
 * @brief `write(2)`.
 * @param fd The descriptor.
 * @param buffer The bytes.
 * @param size How many.
 * @return As `write`.
 */
inline ssize_t write(int fd, const void* buffer, std::size_t size) {
  return ::write(fd, buffer, size);
}

/**
 * @brief `fsync(2)`.
 * @param fd The descriptor.
 * @return As `fsync`.
 */
inline int fsync(int fd) {
  return ::fsync(fd);
}

/**
 * @brief `ftruncate(2)`.
 * @param fd The descriptor.
 * @param size The new size.
 * @return As `ftruncate`.
 */
inline int ftruncate(int fd, off_t size) {
  return ::ftruncate(fd, size);
}

/**
 * @brief `rename(2)`.
 * @param from The old name.
 * @param to The new name.
 * @return As `rename`.
 */
inline int rename(const char* from, const char* to) {
  return std::rename(from, to);
}

/**
 * @brief `fstat(2)`.
 * @param fd The descriptor.
 * @param info Where the status goes.
 * @return As `fstat`.
 */
inline int fstat(int fd, struct stat* info) {
  return ::fstat(fd, info);
}

/**
 * @brief A point the fault test may kill the process at; nothing here.
 * @param name The point.
 */
inline void kill_point(const char* name) noexcept {
  (void)name;
}
#endif

} // namespace sys
} // namespace cmdatl
#endif
