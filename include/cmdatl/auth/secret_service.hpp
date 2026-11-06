#pragma once

#if defined(CMDATL_LIBSECRET)

#include "../system/process.hpp"
#include "./store.hpp"

#include <libsecret/secret.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cmdatl {
namespace auth {
namespace secret_detail {

/**
 * @brief cmdatl's schema: one attribute, `login`, the login's name.
 * @return The schema.
 */
inline const SecretSchema* schema() {
  static const SecretSchema value = {
    "org.algotradelib.cmdatl", SECRET_SCHEMA_NONE,
    {{"login", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}}
  };
  return &value;
}

/**
 * @brief A cancellable that a separate thread cancels at a deadline, or once SIGINT, SIGTERM or SIGHUP arrives, so
 *   no call into the Secret Service waits longer than the deadline.
 */
class Deadline {
public:
  /**
   * @brief Starts the watch.
   * @param limit The deadline from now.
   */
  explicit Deadline(std::chrono::milliseconds limit) : cancellable_(g_cancellable_new()) {
    watcher_ = std::thread([this, limit] {
      const auto until = std::chrono::steady_clock::now() + limit;
      std::unique_lock<std::mutex> lock(mutex_);
      while (!done_) {
        if (std::chrono::steady_clock::now() >= until || interrupted()) {
          g_cancellable_cancel(cancellable_);
          return;
        }
        wake_.wait_for(lock, std::chrono::milliseconds(100));
      }
    });
  }

  Deadline(const Deadline&) = delete;
  Deadline& operator=(const Deadline&) = delete;

  /** @brief Stops the watch. */
  ~Deadline() {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      done_ = true;
    }
    wake_.notify_all();
    watcher_.join();
    g_object_unref(cancellable_);
  }

  /**
   * @brief The cancellable to pass to each call.
   * @return It.
   */
  GCancellable* get() const noexcept {
    return cancellable_;
  }

  /**
   * @brief Whether the deadline or a signal cancelled it.
   * @return Whether it did.
   */
  bool cancelled() const {
    return g_cancellable_is_cancelled(cancellable_) != 0;
  }

private:
  GCancellable* cancellable_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool done_{false};
  std::thread watcher_;
};

/** @brief Frees a GError when it ends. */
struct Error {
  /** @brief The error, or null. */
  GError* error_{nullptr};

  Error() = default;
  Error(const Error&) = delete;
  Error& operator=(const Error&) = delete;

  /** @brief Frees the error. */
  ~Error() {
    if (error_ != nullptr) {
      g_error_free(error_);
    }
  }

  /**
   * @brief Its message, for a person.
   * @return The message, or empty.
   */
  std::string message() const {
    return error_ != nullptr && error_->message != nullptr ? error_->message : "";
  }
};

/**
 * @brief The attributes naming one login, or every login of the schema.
 * @param name The login, or null for every login.
 * @return The table; the caller unrefs it.
 */
inline GHashTable* attributes(const std::string* name) {
  GHashTable* table = g_hash_table_new(g_str_hash, g_str_equal);
  if (name != nullptr) {
    g_hash_table_insert(table, const_cast<char*>("login"), const_cast<char*>(name->c_str()));
  }
  return table;
}

/**
 * @brief The answer for a failed call: cancelled at the deadline, or the service's error.
 * @param deadline The deadline.
 * @param error The error.
 * @return The answer.
 */
inline StoreAnswer failure(const Deadline& deadline, const Error& error) {
  if (deadline.cancelled()) {
    return {StoreStatus::LOCKED, {}, "the Secret Service did not answer in time, or the command was interrupted"};
  }
  return {StoreStatus::UNAVAILABLE, {}, "Secret Service: " + error.message()};
}

} // namespace secret_detail

/**
 * @brief Whether the session can show a dialog: `DISPLAY` or `WAYLAND_DISPLAY` is set, a hint rather than proof (a
 *   D-Bus session bus also runs in a text or SSH session, so it is not taken as one).
 * @return Whether it can.
 */
inline bool session_has_graphics() {
  const char* x11 = std::getenv("DISPLAY");
  const char* wayland = std::getenv("WAYLAND_DISPLAY");
  return (x11 != nullptr && *x11 != '\0') || (wayland != nullptr && *wayland != '\0');
}

/**
 * @brief Logins in the Secret Service (GNOME Keyring, KWallet), through libsecret: one item per login in the default
 *   collection, schema `org.algotradelib.cmdatl`, attribute `login`.
 *
 * Searches never unlock: a locked item answers `LOCKED`. When nobody can answer a dialog, storing into or removing
 * from a locked collection answers `LOCKED` instead of prompting; every call is cancelled after 10 seconds.
 */
class SecretServiceStore : public CredentialStore {
public:
  /**
   * @brief A store that may prompt only when `interaction` says someone can answer.
   * @param interaction Whether someone can answer a dialog.
   */
  explicit SecretServiceStore(bool interaction) : interaction_(interaction) {}

  std::string kind() const override {
    return "secret-service";
  }

  StoreList list() override {
    using namespace secret_detail;
    StoreList result;
    const Deadline deadline(LIMIT);
    Error error;
    SecretService* service = connect(deadline, error);
    if (service == nullptr) {
      const auto answer = failure(deadline, error);
      result.status_ = answer.status_;
      result.detail_ = answer.detail_;
      return result;
    }
    GHashTable* every = attributes(nullptr);
    GList* items =
      secret_service_search_sync(service, schema(), every, SECRET_SEARCH_ALL, deadline.get(), &error.error_);
    g_hash_table_unref(every);
    if (error.error_ != nullptr) {
      g_object_unref(service);
      const auto answer = failure(deadline, error);
      result.status_ = answer.status_;
      result.detail_ = answer.detail_;
      return result;
    }
    result.status_ = StoreStatus::OK;
    for (GList* at = items; at != nullptr; at = at->next) {
      GHashTable* found = secret_item_get_attributes(SECRET_ITEM(at->data));
      if (found != nullptr) {
        if (const auto* login = static_cast<const char*>(g_hash_table_lookup(found, "login"))) {
          result.names_.emplace_back(login);
        }
        g_hash_table_unref(found);
      }
    }
    g_list_free_full(items, g_object_unref);
    g_object_unref(service);
    std::sort(result.names_.begin(), result.names_.end());
    result.names_.erase(std::unique(result.names_.begin(), result.names_.end()), result.names_.end());
    return result;
  }

  StoreAnswer get(const std::string& name) override {
    using namespace secret_detail;
    const Deadline deadline(LIMIT);
    Error error;
    SecretService* service = connect(deadline, error);
    if (service == nullptr) {
      return failure(deadline, error);
    }
    StoreAnswer answer = find(service, name, true, deadline);
    g_object_unref(service);
    return answer;
  }

  StoreAnswer put(const std::string& name, const std::string& value) override {
    using namespace secret_detail;
    const Deadline deadline(LIMIT);
    Error error;
    SecretService* service = connect(deadline, error);
    if (service == nullptr) {
      return failure(deadline, error);
    }
    if (!interaction_) {
      SecretCollection* collection = secret_collection_for_alias_sync(
        service, SECRET_COLLECTION_DEFAULT, SECRET_COLLECTION_NONE, deadline.get(), &error.error_
      );
      if (collection == nullptr) {
        g_object_unref(service);
        return error.error_ != nullptr ?
          failure(deadline, error) :
          StoreAnswer{StoreStatus::UNAVAILABLE, {}, "the Secret Service has no default collection"};
      }
      const bool locked = secret_collection_get_locked(collection) != 0;
      g_object_unref(collection);
      if (locked) {
        g_object_unref(service);
        return {StoreStatus::LOCKED, {}, "the default collection is locked"};
      }
    }
    SecretValue* secret = secret_value_new(value.c_str(), static_cast<gssize>(value.size()), "text/plain");
    GHashTable* one = attributes(&name);
    const std::string label = "cmdatl " + name;
    const gboolean stored = secret_service_store_sync(
      service, schema(), one, SECRET_COLLECTION_DEFAULT, label.c_str(), secret, deadline.get(), &error.error_
    );
    g_hash_table_unref(one);
    secret_value_unref(secret);
    g_object_unref(service);
    if (!stored) {
      return failure(deadline, error);
    }
    return {StoreStatus::OK, {}, {}};
  }

  StoreAnswer remove(const std::string& name) override {
    using namespace secret_detail;
    const Deadline deadline(LIMIT);
    Error error;
    SecretService* service = connect(deadline, error);
    if (service == nullptr) {
      return failure(deadline, error);
    }
    StoreAnswer found = find(service, name, false, deadline);
    if (found.status_ != StoreStatus::OK) {
      g_object_unref(service);
      return found;
    }
    GHashTable* one = attributes(&name);
    const gboolean cleared = secret_service_clear_sync(service, schema(), one, deadline.get(), &error.error_);
    g_hash_table_unref(one);
    g_object_unref(service);
    if (error.error_ != nullptr) {
      return failure(deadline, error);
    }
    return cleared ? StoreAnswer{StoreStatus::OK, {}, {}} : StoreAnswer{StoreStatus::NOT_FOUND, {}, {}};
  }

private:
  static constexpr std::chrono::milliseconds LIMIT{10000};
  bool interaction_;

  // The service, with a session open; null on failure (`error` says why).
  static SecretService* connect(const secret_detail::Deadline& deadline, secret_detail::Error& error) {
    return secret_service_get_sync(SECRET_SERVICE_OPEN_SESSION, deadline.get(), &error.error_);
  }

  // One login, never unlocking it; its document when `load` is set. A locked item answers LOCKED.
  static StoreAnswer find(
    SecretService* service, const std::string& name, bool load, const secret_detail::Deadline& deadline
  ) {
    using namespace secret_detail;
    Error error;
    GHashTable* one = attributes(&name);
    const auto flags = static_cast<SecretSearchFlags>(SECRET_SEARCH_ALL | (load ? SECRET_SEARCH_LOAD_SECRETS : 0));
    GList* items = secret_service_search_sync(service, schema(), one, flags, deadline.get(), &error.error_);
    g_hash_table_unref(one);
    if (error.error_ != nullptr) {
      return failure(deadline, error);
    }
    if (items == nullptr) {
      return {StoreStatus::NOT_FOUND, {}, {}};
    }
    StoreAnswer answer{StoreStatus::OK, {}, {}};
    SecretItem* item = SECRET_ITEM(items->data);
    if (secret_item_get_locked(item)) {
      answer = {StoreStatus::LOCKED, {}, "the login's collection is locked"};
    } else if (load) {
      SecretValue* secret = secret_item_get_secret(item);
      if (secret == nullptr) {
        answer = {StoreStatus::LOCKED, {}, "the login's secret could not be read"};
      } else {
        gsize length = 0;
        const gchar* bytes = secret_value_get(secret, &length);
        answer.value_.assign(bytes, length);
        secret_value_unref(secret);
      }
    }
    g_list_free_full(items, g_object_unref);
    return answer;
  }
};

} // namespace auth
} // namespace cmdatl

#endif
