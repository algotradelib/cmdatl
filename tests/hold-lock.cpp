// Holds an account's lock for the tests, as another cmdatl process would: `hold-lock DIR MILLISECONDS` locks
// DIR/lock, writes `held` and its process id on stdout, and keeps the lock until the time is up or it is killed.
// Not part of cmdatl: no command gives an agent a way to hold the account for as long as it likes.

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

/**
 * @brief The helper's entry point.
 * @param argc The number of arguments.
 * @param argv The account's directory and how long to hold, in milliseconds.
 * @return 0 after holding, 2 on bad usage, 3 when the lock could not be taken.
 */
int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: hold-lock DIR MILLISECONDS\n");
    return 2;
  }
  const std::string path = std::string(argv[1]) + "/lock";
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0 || ::flock(fd, LOCK_EX) != 0) {
    return 3;
  }
  std::printf("held %d\n", static_cast<int>(::getpid()));
  std::fflush(stdout);
  for (long left = std::atol(argv[2]); left > 0; --left) {
    ::usleep(1000);
  }
  ::close(fd);
  return 0;
}
