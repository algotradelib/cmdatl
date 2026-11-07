#pragma once

// Broker folders for tests: a scratch configuration directory named by XDG_CONFIG_HOME while its owner lives, with
// `cmdatl/brokers/<name>/` folders written private to the user, as the operator would install them.

#include "support.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

#if !defined(_WIN32)
#include <sys/stat.h>

namespace support {

/** @brief A scratch XDG_CONFIG_HOME holding `cmdatl/brokers`, restored when its owner goes. */
class BrokerHome {
public:
  /** @brief Creates it and points XDG_CONFIG_HOME at it. */
  BrokerHome() {
    if (const char* before = std::getenv("XDG_CONFIG_HOME")) {
      before_ = before;
    }
    brokers_ = dir_.path() / "cmdatl" / "brokers";
    std::filesystem::create_directories(brokers_);
    ::chmod((dir_.path() / "cmdatl").c_str(), 0700);
    ::chmod(brokers_.c_str(), 0700);
    ::setenv("XDG_CONFIG_HOME", dir_.path().c_str(), 1);
  }

  BrokerHome(const BrokerHome&) = delete;
  BrokerHome& operator=(const BrokerHome&) = delete;

  /** @brief Restores XDG_CONFIG_HOME. */
  ~BrokerHome() {
    if (before_) {
      ::setenv("XDG_CONFIG_HOME", before_->c_str(), 1);
    } else {
      ::unsetenv("XDG_CONFIG_HOME");
    }
  }

  /**
   * @brief `cmdatl/brokers`.
   * @return The path.
   */
  const std::filesystem::path& brokers() const noexcept {
    return brokers_;
  }

  /**
   * @brief Writes a broker folder: `broker.toml` and `adapter.hpp`, the folder 0700 and the files 0600.
   * @param name The folder's name.
   * @param toml `broker.toml`.
   * @param adapter `adapter.hpp`.
   * @return The folder.
   */
  std::filesystem::path folder(
    const std::string& name, const std::string& toml, const std::string& adapter = "#pragma once\n"
  ) const {
    const auto path = brokers_ / name;
    std::filesystem::create_directories(path);
    ::chmod(path.c_str(), 0700);
    file(path / "broker.toml", toml);
    file(path / "adapter.hpp", adapter);
    return path;
  }

  /**
   * @brief Writes a file, 0600.
   * @param path The file.
   * @param text Its text.
   */
  static void file(const std::filesystem::path& path, const std::string& text) {
    spit(path, text);
    ::chmod(path.c_str(), 0600);
  }

private:
  ScratchDir dir_;
  std::filesystem::path brokers_;
  std::optional<std::string> before_;
};

/** @brief A token-only vendor's `broker.toml`: one secret field, no demo. */
inline const std::string VENDOR_TOML =
  "display = \"A vendor\"\ndata_only = true\n"
  "[[fields]]\nname = \"api_token\"\nprompt = \"API token: \"\nsecret = true\n";

/** @brief A broker's `broker.toml`: a key, a secret, an optional region; a demo environment. */
inline const std::string BROKER_TOML =
  "display = \"A broker\"\ndemo = true\n"
  "[[fields]]\nname = \"appKey\"\nprompt = \"App key: \"\n"
  "[[fields]]\nname = \"appSecret\"\nprompt = \"App secret: \"\nsecret = true\n"
  "[[fields]]\nname = \"region\"\nprompt = \"Region (Enter for kr): \"\n"
  "default = \"kr\"\n";

} // namespace support
#endif
