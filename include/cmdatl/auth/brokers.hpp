#pragma once

#include "../core/reply.hpp"

#include <algotradelib/src/brokers/kis/client.hpp>
#include <algotradelib/src/brokers/kis/config.hpp>
#include <algotradelib/src/brokers/kiwoom/config.hpp>
#include <algotradelib/src/brokers/tossinvest/config.hpp>

#include <optional>
#include <string>
#include <vector>

namespace cmdatl {
namespace auth {

/** @brief One field of a broker's credential document, as login asks for it. */
struct BrokerField {
  /** @brief The field's name in the library's JSON document. */
  std::string json_;
  /** @brief What login asks. */
  std::string prompt_;
  /** @brief Whether it is a secret: read with echo off, never shown. */
  bool secret_{false};
  /** @brief The value Enter gives, for an optional field; none for a required one. */
  std::optional<std::string> default_;
  /** @brief Whether `auth status` shows it masked (an account number). */
  bool shown_masked_{false};
};

/** @brief A broker cmdatl can store a login for. */
struct Broker {
  /** @brief Its name: `--broker`, and the stored login's name. */
  std::string name_;
  /** @brief Its name for a person. */
  std::string display_;
  /** @brief Whether it has a demo environment (모의투자); without one, a login is real only. */
  bool has_demo_{false};
  /** @brief The fields login asks for, in order. */
  std::vector<BrokerField> fields_;
  /** @brief The environment variables the library's `from_env()` reads for it, named in `auth status`. */
  std::vector<std::string> env_vars_;
  /**
   * @brief Reads a canonical document with the library's loaders, as a run will.
   * @throws std::runtime_error with the library's message (the field, never its value).
   */
  void (*read_)(const std::string& text, const std::string& origin){nullptr};
};

/**
 * @brief The brokers, in the order login lists them.
 * @return The brokers.
 */
inline const std::vector<Broker>& brokers() {
  static const std::vector<Broker> list{
    {"kis", "Korea Investment & Securities (KIS)", true,
      {{"app_key", "App key: ", false, std::nullopt, false}, {"app_secret", "App secret: ", true, std::nullopt, false},
        {"account_number", "Account number (8 digits): ", false, std::nullopt, true},
        {"account_product_code", "Account product code (2 digits, Enter for 01): ", false, std::string("01"), false}},
      {"KIS_APP_KEY", "KIS_APP_SECRET", "KIS_DEMO", "KIS_CANO", "KIS_ACNT_PRDT_CD"},
      [](const std::string& text, const std::string& origin) {
        algotradelib::kis::KisConfig config;
        config.from_json_text(text, origin);
        algotradelib::kis::KisAccount account;
        account.from_json_text(text, origin);
      }},
    {"kiwoom", "Kiwoom Securities", true,
      {{"appkey", "App key: ", false, std::nullopt, false}, {"secretkey", "Secret key: ", true, std::nullopt, false}},
      {"KIWOOM_APP_KEY", "KIWOOM_SECRET_KEY", "KIWOOM_DEMO"},
      [](const std::string& text, const std::string& origin) {
        algotradelib::kiwoom::KiwoomConfig config;
        config.from_json_text(text, origin);
      }},
    {"toss", "Toss Securities", false,
      {{"client_id", "Client id: ", false, std::nullopt, false},
        {"client_secret", "Client secret: ", true, std::nullopt, false}},
      {"TOSS_CLIENT_ID", "TOSS_CLIENT_SECRET"}, [](const std::string& text, const std::string& origin) {
        algotradelib::tossinvest::TossConfig config;
        config.from_json_text(text, origin);
      }},
  };
  return list;
}

/**
 * @brief Finds a broker by name.
 * @param name The name.
 * @return The broker, or null.
 */
inline const Broker* find_broker(const std::string& name) {
  for (const auto& broker : brokers()) {
    if (broker.name_ == name) {
      return &broker;
    }
  }
  return nullptr;
}

/**
 * @brief The brokers' names, for a message.
 * @return Them, separated by commas.
 */
inline std::string broker_names() {
  std::string names;
  for (const auto& broker : brokers()) {
    names += (names.empty() ? "" : ", ") + broker.name_;
  }
  return names;
}

/**
 * @brief Makes a document canonical: only the broker's fields, each a string, and for a broker with a demo,
 *   `demo` set from the environment chosen.
 * @param broker The broker.
 * @param demo Whether the login is for the demo environment (false for a broker without one).
 * @param document The document as given.
 * @param canonical Where the canonical document goes.
 * @return The refusal's message (naming the field, never a value), or none.
 */
inline std::optional<std::string> canonical_document(
  const Broker& broker, bool demo, const json::object& document, json::object& canonical
) {
  for (const auto& entry : document) {
    const std::string key(entry.key());
    bool known = broker.has_demo_ && key == "demo";
    for (const auto& field : broker.fields_) {
      known = known || field.json_ == key;
    }
    if (!known) {
      // The unknown name is not repeated: it is the caller's text, and could be a pasted secret.
      std::string names;
      for (const auto& field : broker.fields_) {
        names += (names.empty() ? "" : ", ") + field.json_;
      }
      return "a field " + broker.name_ + " does not take; it takes " + names + (broker.has_demo_ ? ", demo" : "");
    }
  }
  if (broker.has_demo_) {
    if (const auto* given = document.if_contains("demo")) {
      if (!given->is_bool()) {
        return std::string("demo is not true or false");
      }
      if (given->get_bool() != demo) {
        return std::string("demo says ") + (given->get_bool() ? "demo" : "real") + ", and the login is for " +
          (demo ? "demo" : "real");
      }
    }
  }
  json::object out;
  for (const auto& field : broker.fields_) {
    const auto* value = document.if_contains(field.json_);
    if (value == nullptr) {
      if (!field.default_) {
        return "the field " + field.json_ + " is missing";
      }
      continue;
    }
    if (!value->is_string()) {
      return "the field " + field.json_ + " is not a string";
    }
    if (value->get_string().empty()) {
      return "the field " + field.json_ + " is empty";
    }
    out[field.json_] = value->get_string();
  }
  if (broker.has_demo_) {
    out["demo"] = demo;
  }
  canonical = std::move(out);
  return std::nullopt;
}

} // namespace auth
} // namespace cmdatl
