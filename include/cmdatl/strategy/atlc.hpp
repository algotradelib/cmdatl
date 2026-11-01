#pragma once

#include "../account/operator.hpp"
#include "../system/process.hpp"

#include <charconv>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace cmdatl {

/** @brief The oldest atlc whose generated code and run spec cmdatl reads. */
inline constexpr const char* MINIMUM_ATLC = "0.0.0";

/** @brief Where cmdatl found atlc, and its version. */
struct Atlc {
  /** @brief What `CMDATL_ATLC` named, as written, or empty. */
  std::string requested_;
  /** @brief The executable, or empty when none was found. */
  std::string path_;
  /** @brief Where the path came from: `CMDATL_ATLC`, `config` (the operator's file), `PATH`, or empty. */
  std::string source_;
  /** @brief What `atlc --version` printed after the name, or empty. */
  std::string version_;
  /**
   * @brief When `CMDATL_ATLC` is unset and the operator's file was named but not read: `config.missing` or
   *   `config.invalid` and why.
   *
   * No atlc is looked for then: one the person did not choose is never run.
   */
  std::optional<ConfigError> config_error_;
};

/**
 * @brief Finds atlc: `CMDATL_ATLC`, else the operator's file's `atlc` key, else `atlc` on `PATH`; then asks it for
 *   its version.
 * @param snapshot The operator's file as this invocation read it, or null when it was not read (`CMDATL_ATLC` set,
 *   or a caller that does not consult it); consulted only when `CMDATL_ATLC` is unset.
 * @return What was found.
 */
inline Atlc find_atlc(const ConfigSnapshot* snapshot = nullptr) {
  Atlc atlc;
  const bool overridden = variable("CMDATL_ATLC").has_value();
  if (!overridden && snapshot != nullptr) {
    if (snapshot->state_ == ConfigSnapshot::State::MISSING || snapshot->state_ == ConfigSnapshot::State::INVALID) {
      // The person named a file: falling through to PATH would run an atlc they did not choose.
      const auto code = snapshot->state_ == ConfigSnapshot::State::MISSING ? "config.missing" : "config.invalid";
      atlc.config_error_ =
        ConfigError{code, snapshot->error_.message_, snapshot->error_.line_, snapshot->error_.column_};
      return atlc;
    }
    if (snapshot->state_ == ConfigSnapshot::State::OK && !snapshot->config_->atlc_.empty()) {
      // A path that does not run stays unresolved: reported, never replaced by another atlc.
      atlc.requested_ = snapshot->config_->atlc_;
      atlc.path_ = snapshot->config_->atlc_;
      atlc.source_ = "config";
      if (const auto printed = capture({atlc.path_, "--version"})) {
        const auto line = printed->substr(0, printed->find('\n'));
        const auto space = line.find(' ');
        atlc.version_ = space == std::string::npos ? line : line.substr(space + 1);
      }
      return atlc;
    }
  }
  if (const auto set = variable("CMDATL_ATLC")) {
    // A path is used as given; a bare name is looked up on PATH only, never in the current directory. An override
    // that resolves to nothing stays unresolved: it is reported, never replaced by another atlc.
    atlc.requested_ = *set;
    atlc.source_ = "CMDATL_ATLC";
    const bool bare_name = set->find('/') == std::string::npos && set->find('\\') == std::string::npos;
    if (!bare_name) {
      atlc.path_ = *set;
    } else if (const auto found = find_on_path(*set)) {
      atlc.path_ = *found;
    } else {
      return atlc;
    }
  } else if (const auto found = find_on_path("atlc")) {
    atlc.path_ = *found;
    atlc.source_ = "PATH";
  } else {
    return atlc;
  }
  if (const auto printed = capture({atlc.path_, "--version"})) {
    // `atlc 0.0.0-9000`
    const auto line = printed->substr(0, printed->find('\n'));
    const auto space = line.find(' ');
    atlc.version_ = space == std::string::npos ? line : line.substr(space + 1);
  }
  return atlc;
}

/**
 * @brief The release part of a version (`0.1.2` of `0.1.2-9000`) as three numbers.
 * @param version The version.
 * @return Major, minor and patch, or nothing when it does not start with three numbers that fit.
 */
inline std::optional<std::vector<int>> release_of(const std::string& version) {
  const auto release = version.substr(0, version.find('-'));
  std::vector<int> parts;
  std::size_t start = 0;
  while (parts.size() < 3) {
    const auto dot = release.find('.', start);
    const auto text = release.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc() || end != text.data() + text.size() || value < 0) {
      return std::nullopt;
    }
    parts.push_back(value);
    if (dot == std::string::npos) {
      break;
    }
    start = dot + 1;
  }
  if (parts.size() != 3) {
    return std::nullopt;
  }
  return parts;
}

/**
 * @brief Whether a version is at least another, comparing releases only.
 * @param version The version found.
 * @param minimum The oldest accepted.
 * @return Whether it is accepted; false when either does not parse.
 */
inline bool at_least(const std::string& version, const std::string& minimum) {
  const auto found = release_of(version);
  const auto wanted = release_of(minimum);
  return found && wanted && *found >= *wanted;
}

} // namespace cmdatl
