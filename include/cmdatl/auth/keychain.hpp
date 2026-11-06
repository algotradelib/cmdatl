#pragma once

#if defined(__APPLE__)

#include "./store.hpp"

// Without this, AssertMacros.h (pulled in by the Security headers) defines `check`, `verify` and `require`.
#ifndef __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES
#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
#endif
#include <CoreFoundation/CoreFoundation.h>
#include <Security/AuthSession.h>
#include <Security/Security.h>

#include <algorithm>
#include <string>
#include <vector>

namespace cmdatl {
namespace auth {
namespace keychain_detail {

/**
 * @brief Owns one Core Foundation reference and releases it.
 * @tparam T The reference type.
 */
template <typename T>
class Cf {
public:
  /**
   * @brief Takes a reference (one from a Create or Copy call).
   * @param ref The reference, or null.
   */
  explicit Cf(T ref = nullptr) noexcept : ref_(ref) {}

  Cf(const Cf&) = delete;
  Cf& operator=(const Cf&) = delete;

  /**
   * @brief Takes another's reference.
   * @param other The other, left empty.
   */
  Cf(Cf&& other) noexcept : ref_(other.ref_) {
    other.ref_ = nullptr;
  }

  Cf& operator=(Cf&&) = delete;

  /** @brief Releases the reference. */
  ~Cf() {
    if (ref_ != nullptr) {
      CFRelease(ref_);
    }
  }

  /**
   * @brief The reference.
   * @return It, or null.
   */
  T get() const noexcept {
    return ref_;
  }

  /**
   * @brief Where a Copy call writes its result; releases what was held first.
   * @return The slot.
   */
  CFTypeRef* out() noexcept {
    if (ref_ != nullptr) {
      CFRelease(ref_);
      ref_ = nullptr;
    }
    return reinterpret_cast<CFTypeRef*>(&ref_);
  }

private:
  T ref_;
};

/**
 * @brief A UTF-8 string as a Core Foundation string.
 * @param text The string.
 * @return The reference.
 */
inline Cf<CFStringRef> string_of(const std::string& text) {
  return Cf<CFStringRef>(CFStringCreateWithBytes(
    kCFAllocatorDefault, reinterpret_cast<const UInt8*>(text.data()), static_cast<CFIndex>(text.size()),
    kCFStringEncodingUTF8, false
  ));
}

/**
 * @brief A Core Foundation string as UTF-8.
 * @param text The string.
 * @return The text, empty if it cannot be converted.
 */
inline std::string text_of(CFStringRef text) {
  if (text == nullptr) {
    return {};
  }
  const auto length = CFStringGetLength(text);
  const auto size = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
  std::string out(static_cast<std::size_t>(size), '\0');
  if (!CFStringGetCString(text, out.data(), size, kCFStringEncodingUTF8)) {
    return {};
  }
  out.resize(std::char_traits<char>::length(out.c_str()));
  return out;
}

/**
 * @brief Maps an OS status to a store answer's status.
 * @param status The status.
 * @return The store status.
 */
inline StoreStatus status_of(OSStatus status) {
  switch (status) {
    case errSecSuccess:
      return StoreStatus::OK;
    case errSecItemNotFound:
      return StoreStatus::NOT_FOUND;
    case errSecInteractionNotAllowed:
    case errSecAuthFailed:
    case errSecUserCanceled:
    case errSecInteractionRequired:
      return StoreStatus::LOCKED;
    default:
      return StoreStatus::UNAVAILABLE;
  }
}

/**
 * @brief An answer from an OS status.
 * @param status The status.
 * @return The answer, its detail the status code.
 */
inline StoreAnswer answer_of(OSStatus status) {
  StoreAnswer answer;
  answer.status_ = status_of(status);
  if (status != errSecSuccess) {
    answer.detail_ = "keychain status " + std::to_string(status);
  }
  return answer;
}

// SecKeychainSetUserInteractionAllowed is deprecated, but it is still the control the file-based keychain honours;
// the SecItem replacements do not stop its dialogs.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

/**
 * @brief Turns the keychain's dialogs off for its lifetime when nobody can answer them, and restores the earlier
 *   setting when it ends.
 */
class NoDialogs {
public:
  /**
   * @brief Turns dialogs off unless interaction is allowed.
   * @param interaction Whether someone can answer a dialog.
   */
  explicit NoDialogs(bool interaction) {
    if (interaction) {
      return;
    }
    Boolean before = true;
    if (SecKeychainGetUserInteractionAllowed(&before) != errSecSuccess) {
      failed_ = true;
      return;
    }
    if (SecKeychainSetUserInteractionAllowed(false) != errSecSuccess) {
      failed_ = true;
      return;
    }
    before_ = before;
    restore_ = true;
  }

  NoDialogs(const NoDialogs&) = delete;
  NoDialogs& operator=(const NoDialogs&) = delete;

  /** @brief Restores the earlier setting. */
  ~NoDialogs() {
    if (restore_) {
      SecKeychainSetUserInteractionAllowed(before_);
    }
  }

  /**
   * @brief Whether dialogs could not be turned off, so nothing may be read.
   * @return Whether they could not.
   */
  bool failed() const noexcept {
    return failed_;
  }

private:
  Boolean before_{true};
  bool restore_{false};
  bool failed_{false};
};

#pragma clang diagnostic pop

} // namespace keychain_detail

/**
 * @brief Whether the session can show a dialog: macOS's session has graphical access (not a remote login without a
 *   window server).
 * @return Whether it can.
 */
inline bool session_has_graphics() {
  SecuritySessionId id = 0;
  SessionAttributeBits bits = 0;
  if (SessionGetInfo(callerSecuritySession, &id, &bits) != errSecSuccess) {
    return false;
  }
  return (bits & sessionHasGraphicAccess) != 0;
}

/**
 * @brief Logins in the macOS keychain: the default keychain (normally the login keychain), file-based, one generic
 *   password per login, service `cmdatl`, account the login's name.
 *
 * An item cmdatl adds lists cmdatl as the program allowed to read it, so another program reading it gets macOS's
 * dialog. An unsigned or ad-hoc-signed cmdatl that changes (an upgrade) may need the operator's approval again.
 */
class KeychainStore : public CredentialStore {
public:
  /**
   * @brief A store that shows macOS's dialogs only when `interaction` says someone can answer them.
   * @param interaction Whether someone can answer a dialog.
   */
  explicit KeychainStore(bool interaction) : interaction_(interaction) {}

  std::string kind() const override {
    return "keychain";
  }

  StoreList list() override {
    using namespace keychain_detail;
    StoreList result;
    const NoDialogs guard(interaction_);
    if (guard.failed()) {
      result.detail_ = "the keychain's dialogs could not be turned off";
      return result;
    }
    auto query = base(nullptr);
    CFDictionarySetValue(query.get(), kSecReturnAttributes, kCFBooleanTrue);
    CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitAll);
    Cf<CFTypeRef> found;
    const auto status = SecItemCopyMatching(query.get(), found.out());
    if (status == errSecItemNotFound) {
      result.status_ = StoreStatus::OK;
      return result;
    }
    if (status != errSecSuccess) {
      const auto answer = answer_of(status);
      result.status_ = answer.status_;
      result.detail_ = answer.detail_;
      return result;
    }
    result.status_ = StoreStatus::OK;
    if (found.get() == nullptr || CFGetTypeID(found.get()) != CFArrayGetTypeID()) {
      return result;
    }
    const auto items = static_cast<CFArrayRef>(found.get());
    for (CFIndex i = 0; i < CFArrayGetCount(items); ++i) {
      const auto item = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(items, i));
      const auto account = static_cast<CFStringRef>(CFDictionaryGetValue(item, kSecAttrAccount));
      auto name = text_of(account);
      if (!name.empty()) {
        result.names_.push_back(std::move(name));
      }
    }
    std::sort(result.names_.begin(), result.names_.end());
    return result;
  }

  StoreAnswer get(const std::string& name) override {
    using namespace keychain_detail;
    const NoDialogs guard(interaction_);
    if (guard.failed()) {
      return {StoreStatus::UNAVAILABLE, {}, "the keychain's dialogs could not be turned off"};
    }
    auto query = base(&name);
    CFDictionarySetValue(query.get(), kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitOne);
    Cf<CFTypeRef> found;
    const auto status = SecItemCopyMatching(query.get(), found.out());
    auto answer = answer_of(status);
    if (status == errSecSuccess && found.get() != nullptr && CFGetTypeID(found.get()) == CFDataGetTypeID()) {
      const auto data = static_cast<CFDataRef>(found.get());
      answer.value_.assign(
        reinterpret_cast<const char*>(CFDataGetBytePtr(data)), static_cast<std::size_t>(CFDataGetLength(data))
      );
    }
    return answer;
  }

  StoreAnswer put(const std::string& name, const std::string& value) override {
    using namespace keychain_detail;
    const NoDialogs guard(interaction_);
    if (guard.failed()) {
      return {StoreStatus::UNAVAILABLE, {}, "the keychain's dialogs could not be turned off"};
    }
    const Cf<CFDataRef> data(CFDataCreate(
      kCFAllocatorDefault, reinterpret_cast<const UInt8*>(value.data()), static_cast<CFIndex>(value.size())
    ));
    auto query = base(&name);
    const Cf<CFMutableDictionaryRef> change(CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
    ));
    CFDictionarySetValue(change.get(), kSecValueData, data.get());
    const auto updated = SecItemUpdate(query.get(), change.get());
    if (updated != errSecItemNotFound) {
      return answer_of(updated);
    }
    const auto label = string_of("cmdatl " + name);
    CFDictionarySetValue(query.get(), kSecValueData, data.get());
    CFDictionarySetValue(query.get(), kSecAttrLabel, label.get());
    return answer_of(SecItemAdd(query.get(), nullptr));
  }

  StoreAnswer remove(const std::string& name) override {
    using namespace keychain_detail;
    const NoDialogs guard(interaction_);
    if (guard.failed()) {
      return {StoreStatus::UNAVAILABLE, {}, "the keychain's dialogs could not be turned off"};
    }
    auto query = base(&name);
    return answer_of(SecItemDelete(query.get()));
  }

private:
  bool interaction_;
  keychain_detail::Cf<CFStringRef> service_{keychain_detail::string_of("cmdatl")};

  // The query naming cmdatl's items, and one login's when `name` is given.
  keychain_detail::Cf<CFMutableDictionaryRef> base(const std::string* name) const {
    keychain_detail::Cf<CFMutableDictionaryRef> query(CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
    ));
    CFDictionarySetValue(query.get(), kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query.get(), kSecAttrService, service_.get());
    if (name != nullptr) {
      const auto account = keychain_detail::string_of(*name);
      CFDictionarySetValue(query.get(), kSecAttrAccount, account.get());
    }
    return query;
  }
};

} // namespace auth
} // namespace cmdatl

#endif
