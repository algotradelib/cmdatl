#pragma once

// Credentials handed from `cmdatl run` to the driver it becomes, on a pipe at descriptor 3, never on the command line
// or in the environment (both readable by other processes of the same user).
//
// `cmdatl` reads the logins from the system's store (whose access list names `cmdatl`, not the driver, a new binary
// per program shape), writes them here, closes the write end and `execve`s the driver; the driver reads descriptor 3 to
// its end and closes it. The payload is bounded below a pipe's buffer, so the write before the exec never blocks.
//
// Framing: for each document, its length in decimal and a newline, then its bytes. An empty document (length 0) is
// "none". The reader refuses anything else and never repeats what it read.

#include <cerrno>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cmdatl {
namespace sys {

/** @brief The descriptor the driver reads its credentials from. */
inline constexpr int CREDENTIAL_FD = 3;

/** @brief The most a payload may hold: under the smallest pipe buffer (macOS 16 KiB), so one write completes. */
inline constexpr std::size_t CREDENTIAL_PAYLOAD_MAX = 15 * 1024;

/**
 * @brief Frames documents for the pipe.
 * @param documents The documents, in the order the reader expects; an empty one means none.
 * @return The payload, or nothing when it would exceed `CREDENTIAL_PAYLOAD_MAX`.
 */
inline std::optional<std::string> frame_credentials(const std::vector<std::string>& documents) {
  std::string payload;
  for (const auto& document : documents) {
    payload += std::to_string(document.size());
    payload += '\n';
    payload += document;
  }
  if (payload.size() > CREDENTIAL_PAYLOAD_MAX) {
    return std::nullopt;
  }
  return payload;
}

/**
 * @brief Reads framed documents from a payload.
 * @param payload The bytes read.
 * @param count How many documents the payload must hold, no more and no fewer.
 * @return The documents, or nothing when the payload is malformed or truncated.
 */
inline std::optional<std::vector<std::string>> unframe_credentials(const std::string& payload, std::size_t count) {
  std::vector<std::string> documents;
  std::size_t at = 0;
  while (documents.size() < count) {
    const auto end = payload.find('\n', at);
    if (end == std::string::npos || end == at || end - at > 5) {
      return std::nullopt;
    }
    std::size_t length = 0;
    for (std::size_t i = at; i < end; ++i) {
      if (payload[i] < '0' || payload[i] > '9') {
        return std::nullopt;
      }
      length = length * 10 + static_cast<std::size_t>(payload[i] - '0');
    }
    at = end + 1;
    if (length > payload.size() - at) {
      return std::nullopt;
    }
    documents.push_back(payload.substr(at, length));
    at += length;
  }
  if (at != payload.size()) {
    return std::nullopt;
  }
  return documents;
}

#if !defined(_WIN32)
/**
 * @brief Writes a payload into a new pipe and leaves its read end at `CREDENTIAL_FD`, open across an exec; the write end
 *   is closed, so the reader sees the end of the payload.
 * @param payload The payload, at most `CREDENTIAL_PAYLOAD_MAX` bytes.
 * @return 0, or the error number (nothing is left open on failure).
 */
inline int stage_credentials(const std::string& payload) {
  if (payload.size() > CREDENTIAL_PAYLOAD_MAX) {
    return EMSGSIZE;
  }
  int fds[2];
  if (::pipe(fds) != 0) {
    return errno;
  }
  const auto fail = [&fds](int error) {
    ::close(fds[0]);
    if (fds[1] >= 0) {
      ::close(fds[1]);
    }
    return error;
  };
  std::size_t written = 0;
  while (written < payload.size()) {
    const auto n = ::write(fds[1], payload.data() + written, payload.size() - written);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return fail(n < 0 ? errno : EIO);
    }
    written += static_cast<std::size_t>(n);
  }
  ::close(fds[1]);
  fds[1] = -1;
  if (fds[0] != CREDENTIAL_FD) {
    if (::dup2(fds[0], CREDENTIAL_FD) < 0) {
      return fail(errno);
    }
    ::close(fds[0]);
    fds[0] = CREDENTIAL_FD;
  }
  const int flags = ::fcntl(CREDENTIAL_FD, F_GETFD);
  if (flags < 0 || ::fcntl(CREDENTIAL_FD, F_SETFD, flags & ~FD_CLOEXEC) != 0) {
    return fail(errno);
  }
  return 0;
}

/**
 * @brief Reads the payload from `CREDENTIAL_FD` to its end, then closes the descriptor.
 * @return The payload, or nothing when the descriptor is not open, is not a pipe, or holds more than
 *   `CREDENTIAL_PAYLOAD_MAX`.
 */
inline std::optional<std::string> read_staged_credentials() {
  struct stat info{};
  if (::fstat(CREDENTIAL_FD, &info) != 0) {
    return std::nullopt;
  }
  if (!S_ISFIFO(info.st_mode)) {
    // Whatever else sits at descriptor 3 is not the logins: closed, so the strategy never inherits it either.
    ::close(CREDENTIAL_FD);
    return std::nullopt;
  }
  std::string payload;
  char buffer[4096];
  bool ok = true;
  for (;;) {
    const auto n = ::read(CREDENTIAL_FD, buffer, sizeof(buffer));
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0) {
      ok = false;
      break;
    }
    if (n == 0) {
      break;
    }
    payload.append(buffer, static_cast<std::size_t>(n));
    if (payload.size() > CREDENTIAL_PAYLOAD_MAX) {
      ok = false;
      break;
    }
  }
  ::close(CREDENTIAL_FD);
  if (!ok) {
    return std::nullopt;
  }
  return payload;
}
#endif

} // namespace sys
} // namespace cmdatl
