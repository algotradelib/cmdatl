#pragma once

#include <boost/json.hpp>

#include <charconv>
#include <cstddef>
#include <map>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace cmdatl {

namespace json = boost::json;

/** @brief The exit codes every command shares. */
enum class Exit : int {
  OK = 0,
  FAILED = 1,
  USAGE = 2,
  REFUSED = 3,
  UNKNOWN = 4,
  PROGRAM = 5,
  INTERRUPTED = 130,
};

/** @brief How a reply is written. */
enum class Format {
  JSON,
  JSONL,
  TEXT,
};

/** @brief One command's answer: the body and the exit code. */
struct Reply {
  /** @brief The reply's fields; `env` is cmdatl's to set, and a body's own `env` is replaced. */
  json::object body_;
  /** @brief The exit code. */
  Exit exit_{Exit::OK};
  /**
   * @brief A text document (a help page, a guide), written as it is on a terminal and as the `text` field in JSON.
   */
  std::optional<std::string> text_;
};

/**
 * @brief A failure reply: `result`, and one `error` object with a stable dotted code.
 * @param exit The exit code.
 * @param code The error code, such as `usage.unknown_command`.
 * @param message What went wrong, for a person.
 * @param next What to do next, or empty.
 * @param retry Whether the same call may succeed later unchanged.
 * @return The reply.
 */
inline Reply error_reply(
  Exit exit,
  const std::string& code,
  const std::string& message,
  const std::string& next = "",
  bool retry = false
) {
  Reply reply;
  reply.exit_ = exit;
  reply.body_["result"] = exit == Exit::REFUSED ? "refused" : (exit == Exit::UNKNOWN ? "unknown" : "failed");
  json::object error;
  error["code"] = code;
  error["message"] = message;
  error["retry"] = retry;
  if (!next.empty()) {
    error["next"] = next;
  }
  reply.body_["error"] = std::move(error);
  return reply;
}

/**
 * @brief Splits a text on a separator.
 * @param text The text.
 * @param separator The separator.
 * @return The parts, empty ones kept.
 */
inline std::vector<std::string> split(const std::string& text, char separator) {
  std::vector<std::string> parts;
  std::string part;
  std::istringstream in(text);
  while (std::getline(in, part, separator)) {
    parts.push_back(part);
  }
  if (!text.empty() && text.back() == separator) {
    parts.emplace_back();
  }
  return parts;
}

/**
 * @brief A whole number written in decimal digits only, without overflow.
 * @param text The text.
 * @return The number, or nothing when the text is not digits or does not fit.
 */
inline std::optional<std::size_t> parse_index(const std::string& text) {
  std::size_t value = 0;
  if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
    return std::nullopt;
  }
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc() || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

/**
 * @brief The value at a dotted path (`positions.0.quantity`), indices selecting array elements.
 * @param value The value to read.
 * @param path The path.
 * @return The value, or nothing when the path does not exist.
 */
inline std::optional<json::value> value_at(const json::value& value, const std::string& path) {
  const json::value* at = &value;
  for (const auto& key : split(path, '.')) {
    if (at->is_object()) {
      const auto* found = at->as_object().if_contains(key);
      if (found == nullptr) {
        return std::nullopt;
      }
      at = found;
    } else if (at->is_array()) {
      const auto index = parse_index(key);
      if (!index || *index >= at->as_array().size()) {
        return std::nullopt;
      }
      at = &at->as_array()[*index];
    } else {
      return std::nullopt;
    }
  }
  return *at;
}

/** @brief The fields `--fields` asked for, as a tree: a node either keeps its whole value or selects inside it. */
struct Selection {
  /** @brief Keep the whole value (a field named without anything deeper wins over deeper ones). */
  bool whole_{false};
  /** @brief The fields selected inside, by key. */
  std::map<std::string, Selection> children_;
};

/**
 * @brief Builds the selection tree from field paths; shared prefixes merge, and order does not matter.
 * @param fields The field paths, such as `positions.symbol`.
 * @return The tree.
 */
inline Selection selection_of(const std::vector<std::string>& fields) {
  Selection root;
  for (const auto& field : fields) {
    Selection* at = &root;
    for (const auto& key : split(field, '.')) {
      at = &at->children_[key];
    }
    at->whole_ = true;
  }
  return root;
}

/**
 * @brief Keeps only the selected fields; inside an array the same selection applies to every element.
 * @param value The value.
 * @param selection The selection.
 * @return The projection.
 */
inline json::value project(const json::value& value, const Selection& selection) {
  if (selection.whole_) {
    return value;
  }
  if (value.is_array()) {
    json::array out;
    for (const auto& element : value.as_array()) {
      out.push_back(project(element, selection));
    }
    return out;
  }
  if (!value.is_object()) {
    return value;
  }
  json::object out;
  for (const auto& child : selection.children_) {
    if (const auto* found = value.as_object().if_contains(child.first)) {
      out[child.first] = project(*found, child.second);
    }
  }
  return out;
}

/**
 * @brief Whether a dotted path exists in a value; inside an array it exists when some element has it, and an empty
 *   array has every path (a legitimately empty list is not a missing field).
 * @param value The value.
 * @param keys The path's keys.
 * @param at The key to look at next.
 * @return Whether it exists.
 */
inline bool present(const json::value& value, const std::vector<std::string>& keys, std::size_t at = 0) {
  if (at == keys.size()) {
    return true;
  }
  if (value.is_array()) {
    const auto& elements = value.as_array();
    if (elements.empty()) {
      return true;
    }
    for (const auto& element : elements) {
      if (present(element, keys, at)) {
        return true;
      }
    }
    return false;
  }
  if (value.is_object()) {
    const auto* found = value.as_object().if_contains(keys[at]);
    return found != nullptr && present(*found, keys, at + 1);
  }
  return false;
}

/**
 * @brief Keeps only the named fields (`a`, `a.b`), dotted fields selecting inside objects and every array element.
 * @param value The value.
 * @param fields The field paths.
 * @return The selection.
 */
inline json::value select_fields(const json::value& value, const std::vector<std::string>& fields) {
  return project(value, selection_of(fields));
}

/**
 * @brief A scalar as text: strings without quotes, other values as compact JSON.
 * @param value The value.
 * @return The text.
 */
inline std::string bare(const json::value& value) {
  if (value.is_string()) {
    return std::string(value.as_string());
  }
  return json::serialize(value);
}

/**
 * @brief Writes a value as indented `key: value` lines for a person.
 * @param out The stream.
 * @param value The value.
 * @param indent The indentation.
 */
inline void write_text(std::ostream& out, const json::value& value, int indent = 0) {
  const std::string pad(static_cast<std::size_t>(indent), ' ');
  if (value.is_object()) {
    for (const auto& entry : value.as_object()) {
      const auto& item = entry.value();
      if (item.is_object() || (item.is_array() && !item.as_array().empty())) {
        out << pad << entry.key() << ":\n";
        write_text(out, item, indent + 2);
      } else {
        out << pad << entry.key() << ": " << (item.is_array() ? "(none)" : bare(item)) << "\n";
      }
    }
  } else if (value.is_array()) {
    for (const auto& element : value.as_array()) {
      if (element.is_object()) {
        out << pad << "-\n";
        write_text(out, element, indent + 2);
      } else {
        out << pad << "- " << bare(element) << "\n";
      }
    }
  } else {
    out << pad << bare(value) << "\n";
  }
}

/** @brief How the caller asked to see a reply. */
struct Rendering {
  /** @brief The format. */
  Format format_{Format::JSON};
  /** @brief `--fields`, or empty for every field. */
  std::vector<std::string> fields_;
  /** @brief `--value`: one value printed bare. */
  std::optional<std::string> value_;
};

/**
 * @brief The reply as written: `env` first and set by cmdatl, then the body.
 * @param reply The reply.
 * @param env The environment.
 * @return The full object.
 */
inline json::object envelope(const Reply& reply, const std::string& env) {
  json::object body;
  body["env"] = env;
  for (const auto& entry : reply.body_) {
    if (entry.key() != "env") {
      body[entry.key()] = entry.value();
    }
  }
  if (reply.text_) {
    body["text"] = *reply.text_;
  }
  return body;
}

/**
 * @brief Whether a reply is a failure (it carries an `error` object).
 * @param reply The reply.
 * @return Whether it is.
 */
inline bool is_failure(const Reply& reply) {
  return reply.body_.contains("error");
}

/**
 * @brief Writes a reply as the caller asked.
 *
 * A failure is always written whole, so its code and recovery facts are never lost to a selection. A `--value` or
 * `--fields` path missing from a reply the command already produced does not turn into a usage error: the whole
 * reply is written with a `warning`, and the command's own exit code stands, because the command may have done
 * something (`cli.hpp` refuses a path no reply of the command can have before the command runs).
 *
 * @param out The stream for the reply.
 * @param reply The reply.
 * @param rendering How to write it.
 * @param env The reply's environment (`offline`, `paper`, `demo`, `live`).
 * @return The exit code the process should end with.
 */
inline Exit write_reply(std::ostream& out, const Reply& reply, const Rendering& rendering, const std::string& env) {
  auto body = envelope(reply, env);
  const bool failed = body.contains("error");
  json::value shown = body;
  if (!failed && rendering.value_) {
    if (const auto found = value_at(body, *rendering.value_)) {
      out << bare(*found) << "\n";
      return reply.exit_;
    }
    body["warning"] = json::object{
      {"code", "usage.no_such_field"},
      {"message", "the reply has no field " + *rendering.value_ + "; the whole reply follows"},
    };
    shown = body;
  } else if (!failed && !rendering.fields_.empty()) {
    json::array missing;
    for (const auto& field : rendering.fields_) {
      if (!present(body, split(field, '.'))) {
        missing.push_back(json::value(field));
      }
    }
    if (!missing.empty()) {
      body["warning"] = json::object{
        {"code", "usage.no_such_field"},
        {"message", "the reply lacks some of the fields asked for; the whole reply follows"},
        {"fields", missing},
      };
      shown = body;
    } else {
      auto fields = rendering.fields_;
      fields.push_back("env");
      fields.push_back("result");
      shown = select_fields(body, fields);
    }
  }
  if (rendering.format_ != Format::TEXT) {
    out << json::serialize(shown) << "\n";
    return reply.exit_;
  }
  if (!failed && reply.text_ && rendering.fields_.empty() && !rendering.value_) {
    // The document as it is, then any other field beside it (an outcome, a note), never hidden by the document.
    out << *reply.text_;
    if (!reply.text_->empty() && reply.text_->back() != '\n') {
      out << "\n";
    }
    json::object rest;
    for (const auto& entry : body) {
      if (entry.key() != "env" && entry.key() != "text") {
        rest[entry.key()] = entry.value();
      }
    }
    if (!rest.empty()) {
      out << "\n";
      write_text(out, rest);
    }
    return reply.exit_;
  }
  if (failed) {
    const auto& error = body.at("error").as_object();
    out << "error: " << bare(error.at("message")) << "\n";
    out << "  code: " << bare(error.at("code")) << "\n";
    if (const auto* next = error.if_contains("next")) {
      out << "  next: " << bare(*next) << "\n";
    }
    out << "  env: " << env << "\n";
    for (const auto& entry : body) {
      if (entry.key() != "env" && entry.key() != "error" && entry.key() != "result") {
        out << "  " << entry.key() << ": " << bare(entry.value()) << "\n";
      }
    }
    return reply.exit_;
  }
  write_text(out, shown);
  return reply.exit_;
}

/**
 * @brief The stream a reply goes to: a failure written as text goes to stderr for a person (clig.dev: messages on
 *   stderr); everything else, a failure in JSON included, goes to stdout, the one stream a program parses.
 * @param reply The reply.
 * @param rendering How it is written.
 * @param out stdout.
 * @param err stderr.
 * @return The stream.
 */
inline std::ostream& stream_for(const Reply& reply, const Rendering& rendering, std::ostream& out, std::ostream& err) {
  return is_failure(reply) && rendering.format_ == Format::TEXT ? err : out;
}

/**
 * @brief Flushes a stream and throws when anything written to it was lost: a stream that does not throw by itself
 *   (`std::cout`) reports a failed write only through its state, and a buffered failure only when flushed.
 * @param stream The stream.
 */
inline void require_written(std::ostream& stream) {
  stream.flush();
  if (!stream) {
    throw std::runtime_error("the reply could not be written");
  }
}

/** @brief Where a command writes: its final reply, and, for a stream, one event at a time before it. */
class Output {
public:
  /**
   * @brief Sets up the output of one command.
   * @param out stdout.
   * @param err stderr, for failures written as text and for progress.
   * @param rendering How the caller asked to see replies.
   * @param env The command's environment.
   */
  Output(std::ostream& out, std::ostream& err, Rendering rendering, std::string env)
  : out_(out), err_(err), rendering_(std::move(rendering)), env_(std::move(env)) {}

  /**
   * @brief Writes one event of a stream at once (a JSON line, or a text line on a terminal), flushed.
   * @param event The event; it should carry `event` (its kind) and `at` (its time).
   * @throws std::runtime_error When the event could not be written.
   */
  void emit(const json::object& event) {
    Reply line;
    line.body_ = event;
    auto rendering = rendering_;
    rendering.value_.reset();
    write_reply(out_, line, rendering, env_);
    require_written(out_);
  }

  /**
   * @brief Writes a progress message at once, flushed: a line on stderr in text and `json`, a
   *   `{"event":"progress","message":...}` line on stdout in `jsonl`, never selected by `--fields` or `--value`.
   * @param message The message.
   * @throws std::runtime_error When it could not be written.
   */
  void progress(const std::string& message) {
    if (rendering_.format_ == Format::JSONL) {
      const json::object event{{"env", env_}, {"event", "progress"}, {"message", message}};
      out_ << json::serialize(event) << "\n";
      require_written(out_);
      return;
    }
    err_ << message << "\n";
    require_written(err_);
  }

  /**
   * @brief Writes the command's final reply, flushed.
   * @param reply The reply.
   * @return The exit code.
   * @throws std::runtime_error When the reply could not be written (`run` turns that into its last resort).
   */
  Exit finish(const Reply& reply) {
    auto& stream = stream_for(reply, rendering_, out_, err_);
    const auto exit = write_reply(stream, reply, rendering_, env_);
    require_written(stream);
    return exit;
  }

  /**
   * @brief How the caller asked to see replies.
   * @return The rendering.
   */
  const Rendering& rendering() const noexcept { return rendering_; }

  /**
   * @brief The command's environment.
   * @return The environment.
   */
  const std::string& env() const noexcept { return env_; }

private:
  std::ostream& out_;
  std::ostream& err_;
  Rendering rendering_;
  std::string env_;
};

}  // namespace cmdatl
