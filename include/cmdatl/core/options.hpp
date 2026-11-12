#pragma once

// A broker folder's options: what its `broker.toml` declares (`[[options]]`), and the values a run spec's
// `[bars.options]` or the operator's `[account.options]` give, checked against the declarations before anything is
// built and without running the adapter. Flat scalars only and no defaults; the adapter receives them as a JSON
// object's text, typed as declared.

#include "./toml.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace cmdatl {
namespace options {

namespace json = boost::json;

/** @brief The most options a broker folder may declare. */
inline constexpr std::size_t MAX_OPTIONS = 32;
/** @brief The longest options' JSON text handed to a driver, in bytes (a driver argument's bound). */
inline constexpr std::size_t MAX_TEXT = 4096;

/** @brief What an option holds, in the order of `TYPE_NAMES`. */
enum class Type {
  /** @brief A string. */
  STRING,
  /** @brief A whole number within 64 bits. */
  INTEGER,
  /** @brief A finite number. */
  NUMBER,
  /** @brief `true` or `false`. */
  BOOLEAN,
};

/** @brief One option a broker folder declares. */
struct Declared {
  /** @brief Its name. */
  std::string name_;
  /** @brief What it holds. */
  Type type_{Type::STRING};
  /** @brief Whether a run must give it. */
  bool required_{false};
  /** @brief The values it may take, or empty for any of its type (a string's or an integer's only). */
  std::vector<json::value> choices_;
};

/** @brief A value given in the operator's file, typed as written, with its place. */
struct Given {
  /** @brief The option's name. */
  std::string name_;
  /** @brief The value. */
  json::value value_;
  /** @brief Where it was written. */
  toml::source_position at_{};
};

/** @brief Each type's name in `broker.toml`. */
inline constexpr const char* TYPE_NAMES[] = {"string", "integer", "number", "boolean"};

/**
 * @brief An option's type for a message.
 * @param type The type.
 * @return `a string`, `an integer`, `a number` or `true or false`.
 */
inline std::string type_text(Type type) {
  static const char* const words[] = {"a string", "an integer", "a number", "true or false"};
  return words[static_cast<int>(type)];
}

/**
 * @brief Whether a name is an identifier: a letter or `_`, then letters, digits or `_`, 64 at most.
 * @param key The name.
 * @return Whether it is.
 */
inline bool identifier(const std::string& key) {
  const auto letter = [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
  };
  return !key.empty() && key.size() <= 64 && letter(key[0]) && std::all_of(key.begin(), key.end(), [&](char c) {
    return letter(c) || (c >= '0' && c <= '9');
  });
}

/**
 * @brief A value as the adapter receives it, from a TOML node: a string, an integer, a float or a boolean.
 * @param node The node.
 * @param out Set to the value.
 * @return Whether the node is one of those.
 */
inline bool from_node(const toml::node& node, json::value& out) {
  if (const auto* s = node.as_string()) {
    out.emplace_string() = s->get();
  } else if (const auto* i = node.as_integer()) {
    out = static_cast<std::int64_t>(i->get());
  } else if (const auto* f = node.as_floating_point(); f != nullptr && std::isfinite(f->get())) {
    out = f->get();
  } else if (const auto* b = node.as_boolean()) {
    out = b->get();
  } else {
    return false;
  }
  return true;
}

/**
 * @brief An integer's text as a whole number: an optional `-`, then digits with no leading zero, within 64 bits.
 * @param text The text.
 * @return The number, or nothing.
 */
inline std::optional<std::int64_t> integer_of(const std::string& text) {
  const auto digits = text.size() > 0 && text[0] == '-' ? text.substr(1) : text;
  if (digits.empty() || (digits.size() > 1 && digits[0] == '0')) {
    return std::nullopt;
  }
  std::int64_t value = 0;
  const auto end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), end, value);
  if (parsed.ec != std::errc() || parsed.ptr != end) {
    return std::nullopt;
  }
  return value;
}

/**
 * @brief A command-line word typed by its option's declaration: a string as it is, an integer by integer syntax, a
 *   number by a decimal's, a boolean `true` or `false`.
 * @param declared The declaration.
 * @param word The word.
 * @param out Set to the value.
 * @return Whether the word is of that type.
 */
inline bool from_word(const Declared& declared, const std::string& word, json::value& out) {
  const auto integer = integer_of(word);
  // A decimal's characters only: strtod alone would read `inf`, `nan` and hexadecimal.
  char* end = nullptr;
  const bool decimal = !word.empty() && word.find_first_not_of("0123456789+-.eE") == std::string::npos &&
    std::isfinite(std::strtod(word.c_str(), &end)) && end == word.c_str() + word.size();
  if (declared.type_ == Type::STRING) {
    out.emplace_string() = word;
  } else if ((declared.type_ == Type::INTEGER || declared.type_ == Type::NUMBER) && integer) {
    out = *integer;
  } else if (declared.type_ == Type::NUMBER && decimal) {
    out = std::strtod(word.c_str(), nullptr);
  } else if (declared.type_ == Type::BOOLEAN && (word == "true" || word == "false")) {
    out = word == "true";
  } else {
    return false;
  }
  return true;
}

/**
 * @brief Checks a value against the declarations: the option declared, its type (a number takes an integer too, an
 *   integer no float), its choices.
 * @param declared The declarations.
 * @param key The option's full name, for the message (`bars.options.NAME`).
 * @param name The option's name.
 * @param value The value.
 * @return What is wrong, or nothing.
 */
inline std::optional<std::string> check(
  const std::vector<Declared>& declared, const std::string& key, const std::string& name, const json::value& value
) {
  const auto found = std::find_if(declared.begin(), declared.end(), [&name](const Declared& d) {
    return d.name_ == name;
  });
  if (found == declared.end()) {
    std::string names;
    for (const auto& d : declared) {
      names += (names.empty() ? "" : ", ") + d.name_;
    }
    return "`" + key + "` is not an option the broker folder's broker.toml declares (" +
      (names.empty() ? std::string("it declares none") : "it declares " + names) + ")";
  }
  const bool fits = found->type_ == Type::STRING ?
    value.is_string() :
    found->type_ == Type::INTEGER ?
    value.is_int64() :
    found->type_ == Type::NUMBER ?
    value.is_int64() || value.is_double() :
    value.is_bool();
  if (!fits) {
    return "`" + key + "` is " + type_text(found->type_) + " (broker.toml), not " + json::serialize(value);
  }
  if (
    !found->choices_.empty() &&
    std::find(found->choices_.begin(), found->choices_.end(), value) == found->choices_.end()
  ) {
    std::string list;
    for (const auto& choice : found->choices_) {
      list += (list.empty() ? "" : ", ") + json::serialize(choice);
    }
    return "`" + key + "` is one of " + list + " (broker.toml), not " + json::serialize(value);
  }
  return std::nullopt;
}

/**
 * @brief The required options a table leaves out.
 * @param declared The declarations.
 * @param given The names given.
 * @return Their names, in the declarations' order.
 */
inline std::vector<std::string> missing(const std::vector<Declared>& declared, const std::set<std::string>& given) {
  std::vector<std::string> out;
  for (const auto& d : declared) {
    if (d.required_ && given.count(d.name_) == 0) {
      out.push_back(d.name_);
    }
  }
  return out;
}

/**
 * @brief The options as the adapter receives them: a JSON object's text, its keys sorted; `{}` for none.
 * @param values The names and values.
 * @return The text.
 */
inline std::string text_of(std::vector<std::pair<std::string, json::value>> values) {
  std::sort(values.begin(), values.end(), [](const auto& a, const auto& b) {
    return a.first < b.first;
  });
  json::object object;
  for (auto& [name, value] : values) {
    object[name] = std::move(value);
  }
  return json::serialize(object);
}

/**
 * @brief The operator file's options as the adapter receives them.
 * @param given The values.
 * @return The text.
 */
inline std::string text_of(const std::vector<Given>& given) {
  std::vector<std::pair<std::string, json::value>> values;
  for (const auto& g : given) {
    values.emplace_back(g.name_, g.value_);
  }
  return text_of(std::move(values));
}

/**
 * @brief Reads `broker.toml`'s `[[options]]`, strictly: each with `name` (an identifier, given once, and not one of the
 *   login's fields, so a credential is never an option), `type`, `required`, and optionally `choices` (for a string or
 *   an integer: values of that type).
 * @param root The parsed file.
 * @param fields The login's field names.
 * @param out Set to the declarations.
 * @return The refusal, with its line and column, or nothing.
 */
inline std::optional<config_detail::Refusal> read_declared(
  const toml::table& root, const std::set<std::string>& fields, std::vector<Declared>& out
) {
  using config_detail::refuse;
  out.clear();
  const auto* node = root.get("options");
  if (node == nullptr) {
    return std::nullopt;
  }
  const auto* list = node->as_array();
  if (list == nullptr || list->size() > MAX_OPTIONS || !list->is_array_of_tables()) {
    return refuse(node->source(), "options must be up to " + std::to_string(MAX_OPTIONS) + " [[options]] tables");
  }
  for (const auto& element : *list) {
    const auto& table = *element.as_table();
    if (auto bad = config_detail::only_keys(table, "options", {"name", "type", "required", "choices"})) {
      return bad;
    }
    Declared declared;
    std::optional<std::string> text;
    if (auto bad = config_detail::string_of(table, "name", "options.name", text)) {
      return bad;
    }
    if (!text || !identifier(*text)) {
      return refuse(
        text ? table.get("name")->source() : table.source(),
        "options.name must be a letter or '_', then letters, digits or '_' (64 at most)"
      );
    }
    declared.name_ = *text;
    const auto& name_at = table.get("name")->source();
    if (fields.count(declared.name_) != 0) {
      return refuse(
        name_at, "options.name " + declared.name_ + " is a field of the login: a credential is never an option"
      );
    }
    for (const auto& earlier : out) {
      if (earlier.name_ == declared.name_) {
        return refuse(name_at, "options.name " + declared.name_ + " is given twice");
      }
    }
    text = std::nullopt;
    if (auto bad = config_detail::string_of(table, "type", "options.type", text)) {
      return bad;
    }
    const auto type = std::find(std::begin(TYPE_NAMES), std::end(TYPE_NAMES), text.value_or(""));
    if (type == std::end(TYPE_NAMES)) {
      return refuse(
        text ? table.get("type")->source() : table.source(),
        "options.type must be \"string\", \"integer\", \"number\" or \"boolean\""
      );
    }
    declared.type_ = static_cast<Type>(type - std::begin(TYPE_NAMES));
    const auto* required = table.get("required");
    if (required == nullptr || required->as_boolean() == nullptr) {
      return refuse(
        required == nullptr ? table.source() : required->source(),
        "options.required must be written, true or false: whether a run must give the option"
      );
    }
    declared.required_ = required->as_boolean()->get();
    if (const auto* choices = table.get("choices")) {
      const auto* array = choices->as_array();
      const bool listed = declared.type_ == Type::STRING || declared.type_ == Type::INTEGER;
      if (!listed || array == nullptr || array->empty()) {
        return refuse(choices->source(), "options.choices lists values, for a string or an integer option");
      }
      for (const auto& item : *array) {
        json::value value;
        const bool typed = declared.type_ == Type::STRING ? item.is_string() : item.is_integer();
        if (!typed || !from_node(item, value)) {
          return refuse(item.source(), "options.choices of " + declared.name_ + " are " + type_text(declared.type_));
        }
        declared.choices_.push_back(std::move(value));
      }
    }
    out.push_back(std::move(declared));
  }
  return std::nullopt;
}

} // namespace options
} // namespace cmdatl
