#pragma once

// Broker folders for tests: a scratch project directory, private to the user, holding `adapters/<name>/` folders
// written private to the user, as a user keeps them beside their strategies.

#include "support.hpp"

#include <filesystem>
#include <string>

#if !defined(_WIN32)
#include <sys/stat.h>

namespace support {

/** @brief A scratch project directory holding broker folders under `adapters/`, removed when its owner goes. */
class BrokerFolders {
public:
  /** @brief Creates it, private to the user. */
  BrokerFolders() {
    ::chmod(dir_.path().c_str(), 0700);
  }

  BrokerFolders(const BrokerFolders&) = delete;
  BrokerFolders& operator=(const BrokerFolders&) = delete;

  /**
   * @brief The project directory, canonical.
   * @return The path.
   */
  const std::filesystem::path& root() const noexcept {
    return dir_.path();
  }

  /**
   * @brief Writes a broker folder `adapters/<name>`: `broker.toml` and `adapter.hpp`, the directories 0700 and the
   *   files 0600.
   * @param name The folder's name.
   * @param toml `broker.toml`.
   * @param adapter `adapter.hpp`.
   * @return The folder, canonical.
   */
  std::filesystem::path folder(
    const std::string& name, const std::string& toml, const std::string& adapter = "#pragma once\n"
  ) const {
    const auto path = dir_.path() / "adapters" / name;
    std::filesystem::create_directories(path);
    ::chmod(path.parent_path().c_str(), 0700);
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
