#pragma once

#include <string>
#include <vector>

namespace cmdatl {
namespace auth {

/** @brief How a store answered. */
enum class StoreStatus {
  /** @brief Done, or found. */
  OK,
  /** @brief No such login. */
  NOT_FOUND,
  /** @brief The store needs someone to approve or unlock it, and nobody can be asked. */
  LOCKED,
  /** @brief The store cannot be used (none running, or it failed). */
  UNAVAILABLE,
};

/** @brief A store's answer about one login. */
struct StoreAnswer {
  /** @brief How it answered. */
  StoreStatus status_{StoreStatus::UNAVAILABLE};
  /** @brief The login's document, for a `get` that found it; never shown. */
  std::string value_;
  /** @brief Why it failed, for a person; never a secret (an OS status code, a service's error). */
  std::string detail_;
};

/** @brief A store's list of logins. */
struct StoreList {
  /** @brief How it answered. */
  StoreStatus status_{StoreStatus::UNAVAILABLE};
  /** @brief The logins' names (`kis/demo`), sorted. */
  std::vector<std::string> names_;
  /** @brief Why it failed, for a person. */
  std::string detail_;
};

/**
 * @brief Where logins are kept: one item per login, named `<broker>/<environment>`, holding its canonical document.
 *
 * A store never waits on its own approval or unlock dialog when interaction is not allowed: it answers `LOCKED`.
 */
class CredentialStore {
public:
  /** @brief Destroys the store. */
  virtual ~CredentialStore() = default;

  /**
   * @brief Names the store, for `auth status`.
   * @return `keychain`.
   */
  virtual std::string kind() const = 0;

  /**
   * @brief Lists the logins, without reading their documents.
   * @return The list.
   */
  virtual StoreList list() = 0;

  /**
   * @brief Reads a login's document.
   * @param name The login.
   * @return The answer.
   */
  virtual StoreAnswer get(const std::string& name) = 0;

  /**
   * @brief Stores a login's document, replacing an earlier one.
   * @param name The login.
   * @param value The document.
   * @return The answer.
   */
  virtual StoreAnswer put(const std::string& name, const std::string& value) = 0;

  /**
   * @brief Removes a login.
   * @param name The login.
   * @return The answer.
   */
  virtual StoreAnswer remove(const std::string& name) = 0;
};

} // namespace auth
} // namespace cmdatl
