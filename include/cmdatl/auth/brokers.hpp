#pragma once

#include "../brokers/derive.hpp"
#include "../core/reply.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
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
   * @brief Reads a canonical document with the library's loaders, as a run will; null for a broker folder, whose
   *   document is stored unchecked and read by its adapter at the run.
   * @return The library's identity of the login's key, which names its saved access token.
   * @throws std::runtime_error with the library's message (the field, never its value).
   */
  std::string (*read_)(const std::string& text, const std::string& origin){nullptr};
  /** @brief Whether a login of it saves an access token in the token folder (a broker folder's never does). */
  bool token_{false};
  /** @brief Whether it lists its accounts: `auth login` asks it which account the keys trade and stores the number. */
  bool discovers_{false};
};

/**
 * @brief The brokers, in the order login lists them: the built-ins of the binary's catalog, each named by the user's
 *   word (`toss`, never `tossinvest`).
 * @return The brokers.
 */
inline const std::vector<Broker>& brokers() {
  static const std::vector<Broker> list = [] {
    std::vector<Broker> out;
    for (const auto& entry : ::cmdatl::brokers::catalog()) {
      const auto& facts = *entry.facts_;
      Broker broker;
      broker.name_ = std::string(facts.word_);
      broker.display_ = std::string(facts.display_);
      broker.has_demo_ = entry.has_demo_;
      for (std::size_t i = 0; i < facts.field_count_; ++i) {
        const auto& field = facts.fields_[i];
        broker.fields_.push_back(
          {std::string(field.json_), std::string(field.prompt_), field.secret_,
            field.default_.empty() ? std::nullopt : std::optional<std::string>(std::string(field.default_)),
            field.shown_masked_}
        );
      }
      for (std::size_t i = 0; i < facts.env_var_count_; ++i) {
        broker.env_vars_.emplace_back(facts.env_vars_[i]);
      }
      broker.read_ = entry.read_login_;
      broker.token_ = entry.saves_token_;
      broker.discovers_ = entry.discovers_;
      out.push_back(std::move(broker));
    }
    return out;
  }();
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
 * @brief Makes a document canonical: only the broker's fields, each a string (a broker folder's defaults written in),
 *   and for a broker with a demo, `demo` set from the environment chosen.
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
      // A built-in broker's loader gives its own default; a folder's document is read by nothing before the run, so
      // the default is written into it.
      if (broker.read_ == nullptr) {
        out[field.json_] = *field.default_;
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
