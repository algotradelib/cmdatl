#pragma once

// Strict TOML for cmdatl's own files (the operator's file, a broker folder's `broker.toml`): toml++ with its invariants
// checked, the checks before it parses, and the validator's helpers, each refusal with its line and column.

#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cmdatl {

/** @brief toml++ found one of its own invariants broken by a document: the document is refused. */
class TomlInvariant : public std::runtime_error {
public:
  /**
   * @brief Builds the error.
   * @param what The invariant.
   */
  explicit TomlInvariant(const char* what) : std::runtime_error(what) {}
};

} // namespace cmdatl

// toml++ checks its own invariants with TOML_ASSERT, an abort without NDEBUG and, with it, an assumption the optimizer
// may build on (`TOML_ASSERT_ASSUME`); a table header such as `[~x]` reaches one in toml++ 3.4.0.
// Kept as a check that throws, whatever NDEBUG says, so such a document is refused, never an abort or undefined
// behaviour.
#pragma push_macro("NDEBUG")
#undef NDEBUG
#define TOML_ASSERT(expr) ((expr) ? static_cast<void>(0) : throw ::cmdatl::TomlInvariant("toml++ invariant: " #expr))
#include <toml++/toml.hpp>
#pragma pop_macro("NDEBUG")

namespace cmdatl {
namespace config_detail {

/** @brief A refusal of the validator, with where it applies. */
struct Refusal {
  /** @brief The code. */
  std::string code_;
  /** @brief What is wrong. */
  std::string message_;
  /** @brief Where. */
  toml::source_position at_;
};

/**
 * @brief A refusal at a node.
 * @param where Where.
 * @param message What is wrong.
 * @param code The code.
 * @return The refusal.
 */
inline Refusal refuse(const toml::source_region& where, std::string message, std::string code = "config.invalid") {
  return Refusal{std::move(code), std::move(message), where.begin};
}

/**
 * @brief The name of a TOML node's type, for a message.
 * @param node The node.
 * @return The name.
 */
inline std::string type_name(const toml::node& node) {
  switch (node.type()) {
    case toml::node_type::table:
      return "a table";
    case toml::node_type::array:
      return "an array";
    case toml::node_type::string:
      return "a string";
    case toml::node_type::integer:
      return "an integer";
    case toml::node_type::floating_point:
      return "a float";
    case toml::node_type::boolean:
      return "a boolean";
    case toml::node_type::date:
    case toml::node_type::time:
    case toml::node_type::date_time:
      return "a TOML date or time";
    default:
      return "a value";
  }
}

/**
 * @brief Refuses a key a table does not list: a misspelt optional key would otherwise leave its default in force.
 * @param table The table.
 * @param name The table's name, for the message (empty for the top level).
 * @param allowed The keys it may have.
 * @return The refusal of the first other key, or nothing.
 */
inline std::optional<Refusal> only_keys(
  const toml::table& table, const std::string& name, const std::set<std::string>& allowed
) {
  for (const auto& entry : table) {
    const auto key = std::string(entry.first.str());
    if (allowed.count(key) == 0) {
      return refuse(
        entry.first.source(),
        (name.empty() ? "" : "[" + name + "] ") + "has no key " + key + " (the keys are: " +
          [&] {
            std::string list;
            for (const auto& k : allowed) {
              list += (list.empty() ? "" : ", ") + k;
            }
            return list;
          }() +
          ")"
      );
    }
  }
  return std::nullopt;
}

/**
 * @brief An integer key within a range.
 * @param table The table.
 * @param key The key.
 * @param name The key's full name, for the message.
 * @param low The lowest value.
 * @param high The highest value.
 * @param out Where the value goes when present.
 * @return The refusal, or nothing (also when absent).
 */
inline std::optional<Refusal> integer_in(
  const toml::table& table, const char* key, const std::string& name, std::int64_t low, std::int64_t high,
  std::int64_t& out
) {
  const auto* node = table.get(key);
  if (node == nullptr) {
    return std::nullopt;
  }
  const auto* value = node->as_integer();
  if (value == nullptr) {
    return refuse(node->source(), name + " must be an integer, not " + type_name(*node));
  }
  if (value->get() < low || value->get() > high) {
    return refuse(
      node->source(),
      name + " must be from " + std::to_string(low) + " to " + std::to_string(high) + ", not " +
        std::to_string(value->get())
    );
  }
  out = value->get();
  return std::nullopt;
}

/**
 * @brief A string key.
 * @param table The table.
 * @param key The key.
 * @param name The key's full name, for the message.
 * @param out Where the value goes when present.
 * @return The refusal, or nothing (also when absent).
 */
inline std::optional<Refusal> string_of(
  const toml::table& table, const char* key, const std::string& name, std::optional<std::string>& out
) {
  const auto* node = table.get(key);
  if (node == nullptr) {
    return std::nullopt;
  }
  const auto* value = node->as_string();
  if (value == nullptr) {
    return refuse(node->source(), name + " must be a string, not " + type_name(*node));
  }
  out = value->get();
  return std::nullopt;
}

/**
 * @brief A required table.
 * @param table The parent.
 * @param key The key.
 * @param out Where the table goes.
 * @return The refusal, or nothing.
 */
inline std::optional<Refusal> table_of(const toml::table& table, const char* key, const toml::table*& out) {
  const auto* node = table.get(key);
  if (node == nullptr) {
    return refuse(table.source(), std::string("the file needs a [") + key + "] table");
  }
  out = node->as_table();
  if (out == nullptr) {
    return refuse(node->source(), std::string(key) + " must be a table, not " + type_name(*node));
  }
  return std::nullopt;
}

/** @brief The most segments a key or a table header may have; the operator's file needs two. */
inline constexpr int MAX_KEY_SEGMENTS = 8;

/** @brief The deepest arrays and inline tables may nest; the operator's file needs one level. */
inline constexpr int MAX_VALUE_NESTING = 8;

/**
 * @brief Refuses a character outside ASCII where TOML allows only ASCII, outside strings and comments, before toml++
 *   parses the text: toml++ 3.4 classifies some such characters (U+3001, U+FE70, ...) by a path it marks unreachable,
 *   which is undefined behaviour in an optimized build.
 * @param text The whole file.
 * @return A refusal at the character's line and column, or nothing.
 */
inline std::optional<Refusal> non_ascii_refusal(std::string_view text) {
  std::uint32_t line = 1;
  std::uint32_t column = 1;
  std::size_t i = 0;
  const auto at = [&text, &i](std::string_view token) {
    return text.compare(i, token.size(), token) == 0;
  };
  const auto step = [&]() {
    const auto byte = static_cast<unsigned char>(text[i]);
    if (byte == '\n') {
      ++line;
      column = 1;
    } else if ((byte & 0xC0u) != 0x80u) {
      ++column;
    }
    ++i;
  };
  // Skips a string opened at `i`; a single-line one ends at its quote or at a new line, where toml++ refuses it.
  const auto skip_string = [&](char quote) {
    const bool escapes = quote == '"';
    const std::string_view triple = escapes ? std::string_view("\"\"\"") : std::string_view("'''");
    const bool multi = at(triple);
    for (std::size_t k = 0; k < (multi ? 3u : 1u); ++k) {
      step();
    }
    while (i < text.size()) {
      if (escapes && text[i] == '\\') {
        step();
        if (i < text.size()) {
          step();
        }
      } else if (multi && at(triple)) {
        for (int k = 0; k < 3; ++k) {
          step();
        }
        return;
      } else if (!multi && (text[i] == quote || text[i] == '\n')) {
        if (text[i] == quote) {
          step();
        }
        return;
      } else {
        step();
      }
    }
  };
  // A byte-order mark that opens the file is toml++'s to skip.
  if (at("\xEF\xBB\xBF")) {
    i = 3;
  }
  while (i < text.size()) {
    const char c = text[i];
    if (c == '"' || c == '\'') {
      skip_string(c);
    } else if (c == '#') {
      while (i < text.size() && text[i] != '\n') {
        step();
      }
    } else if ((static_cast<unsigned char>(c) & 0x80u) != 0) {
      return Refusal{
        "config.invalid", "a character outside ASCII outside a string or a comment, which TOML does not allow there",
        {line, column}
      };
    } else {
      step();
    }
  }
  return std::nullopt;
}

/**
 * @brief Bounds how deep the text can nest tables before toml++ parses it: toml++ recurses once per level, and a
 *   dotted key or header 16,000 segments deep, well under 64 KiB, exhausts the stack.
 * @details Scans outside strings and comments: a run of dots not broken by `=`, `,`, a bracket, a brace or a new line
 *   is one key path, of at most `MAX_KEY_SEGMENTS` segments; brackets and braces opened in a value nest at most
 *   `MAX_VALUE_NESTING` deep.
 *   A value has at most one dot outside a string (a float, a time), so no value the file can hold is refused.
 * @param text The whole file.
 * @return A refusal at the first segment or bracket past the bound, or nothing.
 */
inline std::optional<Refusal> nesting_refusal(std::string_view text) {
  std::uint32_t line = 1;
  std::uint32_t column = 0;
  int dots = 0;
  int depth = 0;
  bool header_line = false;
  bool line_blank = true;
  std::size_t i = 0;
  const auto advance = [&](std::size_t count) {
    for (std::size_t k = 0; k < count && i < text.size(); ++k, ++i) {
      const auto byte = static_cast<unsigned char>(text[i]);
      if (byte == '\n') {
        ++line;
        column = 0;
        line_blank = true;
      } else if (byte != ' ' && byte != '\t' && byte != '\r') {
        line_blank = false;
      }
      if (byte != '\n' && (byte & 0xC0u) != 0x80u) {
        ++column;
      }
    }
  };
  const auto at = [&](std::string_view token) {
    return text.compare(i, token.size(), token) == 0;
  };
  // Skips a string opened at `i`, ending a single-line one at a new line as toml++ refuses it there.
  const auto skip_string = [&](char quote) {
    const bool escapes = quote == '"';
    const std::string_view triple = escapes ? std::string_view("\"\"\"") : std::string_view("'''");
    if (at(triple)) {
      advance(3);
      while (i < text.size()) {
        if (escapes && text[i] == '\\') {
          advance(2);
        } else if (at(triple)) {
          advance(3);
          // Up to two quotes may sit just inside the closing delimiter.
          for (int extra = 0; extra < 2 && i < text.size() && text[i] == quote; ++extra) {
            advance(1);
          }
          return;
        } else {
          advance(1);
        }
      }
      return;
    }
    advance(1);
    while (i < text.size() && text[i] != '\n') {
      if (escapes && text[i] == '\\') {
        advance(text.size() > i + 1 && text[i + 1] == '\n' ? 1 : 2);
      } else if (text[i] == quote) {
        advance(1);
        return;
      } else {
        advance(1);
      }
    }
  };
  const auto refuse_here = [&](std::string message) {
    toml::source_position where{};
    where.line = line;
    where.column = column + 1;
    return Refusal{"config.invalid", std::move(message), where};
  };
  while (i < text.size()) {
    const char c = text[i];
    if (c == '"' || c == '\'') {
      skip_string(c);
      continue;
    }
    if (c == '#') {
      while (i < text.size() && text[i] != '\n') {
        advance(1);
      }
      continue;
    }
    if (c == '.') {
      if (++dots >= MAX_KEY_SEGMENTS) {
        return refuse_here("a key or table header has more than " + std::to_string(MAX_KEY_SEGMENTS) + " segments");
      }
    } else if (c == '=' || c == ',' || c == '\n') {
      dots = 0;
      if (c == '\n' && header_line) {
        header_line = false;
        depth = 0;
      }
    } else if (c == '[' || c == '{') {
      dots = 0;
      // A header's brackets open a line outside any value; they name a table and do not nest a value.
      if (c == '[' && depth == 0 && !header_line && line_blank) {
        header_line = true;
      } else if (!header_line && ++depth > MAX_VALUE_NESTING) {
        return refuse_here(
          "arrays and inline tables nest more than " + std::to_string(MAX_VALUE_NESTING) + " levels deep"
        );
      }
    } else if (c == ']' || c == '}') {
      dots = 0;
      if (!header_line && depth > 0) {
        --depth;
      }
    }
    advance(1);
  }
  return std::nullopt;
}

} // namespace config_detail
} // namespace cmdatl
