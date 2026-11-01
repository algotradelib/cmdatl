#pragma once

// `cmdatl guide agent|strategy`: the skills' own text, embedded at build time (`cmdatl/guides.hpp`).

#include "../core/reply.hpp"

#include <cmdatl/guides.hpp>

#include <string>

namespace cmdatl {

/**
 * @brief A SKILL.md without its frontmatter: what follows the line `---` that closes it, leading blank lines dropped.
 * @param file The file, whole.
 * @return The body; the whole file when it opens with no frontmatter.
 */
inline std::string guide_body(const std::string& file) {
  if (file.rfind("---\n", 0) != 0) {
    return file;
  }
  const auto close = file.find("\n---\n", 3);
  if (close == std::string::npos) {
    return file;
  }
  const auto start = file.find_first_not_of('\n', close + 5);
  return start == std::string::npos ? std::string() : file.substr(start);
}

/**
 * @brief The reply of `cmdatl guide NAME`.
 * @param name The guide's name, as the command's last word.
 * @return The body as `text`; a failure for a name with no guide, which the table never passes.
 */
inline Reply guide_reply(const std::string& name) {
  for (const auto& guide : GUIDES) {
    if (name == guide.name_) {
      Reply reply;
      reply.text_ = guide_body(guide.text_);
      return reply;
    }
  }
  return error_reply(Exit::FAILED, "internal.error", "no guide " + name, "report the defect with the command line");
}

} // namespace cmdatl
