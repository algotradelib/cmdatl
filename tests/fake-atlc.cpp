// A stand-in for atlc in cmdatl's tests. What it does is read from the file FAKE_ATLC_SCRIPT names: one step per
// line, `WHEN ACTION [TEXT]`, where WHEN is the command it was called as (`version` for `--version`, `spec`, `run`)
// or `any`; the steps of other commands are skipped. It exits 0 after the last step.

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

/**
 * @brief Writes a whole text, ignoring failures (a closed pipe ends the writer through SIGPIPE, as atlc would).
 * @param fd The descriptor.
 * @param text The text.
 */
void put(int fd, const std::string& text) {
  std::size_t done = 0;
  while (done < text.size()) {
    const auto n = ::write(fd, text.data() + done, text.size() - done);
    if (n <= 0) {
      return;
    }
    done += static_cast<std::size_t>(n);
  }
}

/**
 * @brief Appends a line to a file.
 * @param path The file.
 * @param line The line.
 */
void append(const std::string& path, const std::string& line) {
  std::ofstream out(path, std::ios::app);
  out << line << "\n";
}

/** @brief Sleeps until a signal ends the process. */
[[noreturn]] void block() {
  while (true) {
    ::pause();
  }
}

/**
 * @brief Starts a grandchild that stays in the group and sleeps until killed.
 * @param mode `hold` (keeps the pipes), `ignore-term` (keeps them and ignores SIGTERM) or `ignore-term-closed`
 *   (closes them and ignores SIGTERM).
 * @param pids The file its process id is appended to, before this process goes on.
 */
void grandchild(const std::string& mode, const std::string& pids) {
  int sync[2];
  if (::pipe(sync) != 0) {
    std::exit(90);
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    std::exit(91);
  }
  if (pid == 0) {
    ::close(sync[0]);
    if (mode.find("ignore-term") != std::string::npos) {
      ::signal(SIGTERM, SIG_IGN);
    }
    if (mode.find("closed") != std::string::npos) {
      ::close(STDIN_FILENO);
      ::close(STDOUT_FILENO);
      ::close(STDERR_FILENO);
    }
    append(pids, std::to_string(::getpid()));
    ::close(sync[1]);
    block();
  }
  ::close(sync[1]);
  char byte = 0;
  while (::read(sync[0], &byte, 1) > 0) {
  }
  ::close(sync[0]);
}

} // namespace

/**
 * @brief Runs the steps of the script for the command it was called as.
 * @param argc The number of arguments.
 * @param argv The arguments.
 * @return The exit code.
 */
int main(int argc, char** argv) {
  const char* script = std::getenv("FAKE_ATLC_SCRIPT");
  if (script == nullptr) {
    put(STDERR_FILENO, "fake-atlc: FAKE_ATLC_SCRIPT is not set\n");
    return 99;
  }
  const std::string base(script);
  const std::string called = argc < 2 ? "" : argv[1];
  const std::string mode = called == "--version" ? "version" : called;
  std::ifstream in(base);
  std::string line;
  std::vector<std::string> steps;
  while (std::getline(in, line)) {
    steps.push_back(line);
  }
  for (const auto& step : steps) {
    const auto first = step.find(' ');
    if (first == std::string::npos) {
      continue;
    }
    const auto when = step.substr(0, first);
    if (when != mode && when != "any") {
      continue;
    }
    const auto second = step.find(' ', first + 1);
    const auto action = step.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
    const auto text = second == std::string::npos ? std::string() : step.substr(second + 1);
    const auto number = [&text] {
      return std::atol(text.c_str());
    };
    if (action == "out") {
      put(STDOUT_FILENO, text + "\n");
    } else if (action == "outpart") {
      put(STDOUT_FILENO, text);
    } else if (action == "err") {
      put(STDERR_FILENO, text + "\n");
    } else if (action == "errpart") {
      put(STDERR_FILENO, text);
    } else if (action == "errbytes") {
      put(STDERR_FILENO, std::string(static_cast<std::size_t>(number()), 'e'));
    } else if (action == "errlines") {
      for (long i = 0; i < number(); ++i) {
        put(STDERR_FILENO, "line " + std::to_string(i) + "\n");
      }
    } else if (action == "flood") {
      // Both pipes in turns, more than either pipe holds: a reader that reads only one of them deadlocks.
      const std::string out_chunk(1024, 'o');
      const std::string err_chunk = std::string(99, 'e') + "\n";
      for (long done = 0; done < number(); done += 1024) {
        put(STDOUT_FILENO, out_chunk);
        for (int i = 0; i < 10; ++i) {
          put(STDERR_FILENO, err_chunk);
        }
      }
    } else if (action == "forever") {
      // Output that never ends, a line on stderr every 20 ms.
      for (long i = 0;; ++i) {
        put(STDOUT_FILENO, std::string(4096, 'o'));
        put(STDERR_FILENO, "progress " + std::to_string(i) + "\n");
        ::usleep(20000);
      }
    } else if (action == "sleep") {
      ::usleep(static_cast<useconds_t>(number() * 1000));
    } else if (action == "block") {
      block();
    } else if (action == "exit") {
      return static_cast<int>(number());
    } else if (action == "raise") {
      ::signal(static_cast<int>(number()), SIG_DFL);
      ::kill(::getpid(), static_cast<int>(number()));
      block();
    } else if (action == "ignore-term") {
      ::signal(SIGTERM, SIG_IGN);
    } else if (action == "pid") {
      append(base + ".pids", std::to_string(::getpid()));
    } else if (action == "grandchild") {
      grandchild(text, base + ".pids");
    } else if (action == "touch") {
      append(base + "." + text, "");
    } else if (action == "spoil") {
      // A line no TOML reader takes, appended to a file beside the script (an operator's file rewritten meanwhile).
      append(base + "." + text, "!!! not toml");
    } else if (action == "args") {
      for (int i = 2; i < argc; ++i) {
        append(base + "." + mode + ".args", argv[i]);
      }
    } else if (action == "close") {
      ::close(STDOUT_FILENO);
      ::close(STDERR_FILENO);
    } else {
      put(STDERR_FILENO, "fake-atlc: unknown step " + step + "\n");
      return 98;
    }
  }
  return 0;
}
