// Holds an account's lock for the tests, as another cmdatl process would: `hold-lock DIR` locks DIR/lock, writes
// `held` and its process id on stdout, and keeps the lock until it is killed.
// Not part of cmdatl: no command gives an agent a way to hold the account for as long as it likes.

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cstdio>
#include <string>

/**
 * @brief The helper's entry point.
 * @param argc The number of arguments.
 * @param argv The account's directory.
 * @return 2 on bad usage, 3 when the lock could not be taken; otherwise it holds until killed.
 */
int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: hold-lock DIR\n");
    return 2;
  }
  const std::string path = std::string(argv[1]) + "/lock";
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0 || ::flock(fd, LOCK_EX) != 0) {
    return 3;
  }
  std::printf("held %d\n", static_cast<int>(::getpid()));
  std::fflush(stdout);
  for (;;) {
    ::pause();
  }
}
