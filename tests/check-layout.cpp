// Checks that every header under include/cmdatl/ sits in a module and includes only the modules the table allows.
// Usage: check-layout SOURCE_ROOT, or check-layout --self-test for the lexer's own cases.
// Exit 0 when the tree keeps the table, 1 with one line per problem, 2 on bad usage.
//
// Every #include is read, in every #if branch: the table is a policy, not the host's dependency list. Comments,
// string and character literals and raw strings are skipped, so a commented-out include is not read.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Each module, and the modules its headers may include besides its own. Change it only with the diagram
// (docs/architecture.png).
const std::map<std::string, std::set<std::string>> ALLOWED{
  {"core", {}},
  {"system", {}},
  {"paper", {}},
  {"account", {"core", "system", "paper"}},
  {"orders", {"core", "system", "paper", "account"}},
  {"strategy", {"core", "system", "account"}},
  {"cli", {"core", "system", "paper", "account", "orders", "strategy"}},
};

struct Directive {
  std::size_t line_;
  // The operand as written (`"x.hpp"`, `<x>`, or a macro), or empty with `problem_` set.
  std::string operand_;
  // A spelling the check does not read, refused rather than skipped.
  std::string problem_;
};

// The #include directives of a source text. Spellings the lexer does not model are refused, so nothing is skipped:
// a line splice (backslash-newline), a carriage return, form feed or vertical tab, the digraph `%:` and a byte-order mark
// anywhere, and a comment between `#`, `include` and the operand.
std::vector<Directive> includes_of(const std::string& text) {
  std::vector<Directive> found;
  std::size_t line = 1;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\n') {
      ++line;
    } else if (text[i] == '\\' && (text.compare(i + 1, 1, "\n") == 0 || text.compare(i + 1, 2, "\r\n") == 0)) {
      found.push_back({line, "", "a line splice (backslash-newline), which the layout check does not read"});
    } else if (text[i] == '\r' || text[i] == '\f' || text[i] == '\v') {
      found.push_back({line, "", "a carriage return, form feed or vertical tab, which the layout check does not read"});
    } else if (text.compare(i, 2, "%:") == 0) {
      found.push_back({line, "", "the digraph %:, which the layout check does not read"});
    }
  }
  if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) {
    found.push_back({1, "", "a byte-order mark, which the layout check does not read"});
  }
  line = 1;
  bool line_start = true;
  for (std::size_t i = 0; i < text.size();) {
    const char c = text[i];
    if (c == '\n') {
      ++line;
      line_start = true;
      ++i;
    } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      while (i < text.size() && text[i] != '\n') {
        ++i;
      }
    } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
      const auto end = text.find("*/", i + 2);
      const auto stop = end == std::string::npos ? text.size() : end + 2;
      for (; i < stop; ++i) {
        line += text[i] == '\n' ? 1 : 0;
      }
    } else if (c == 'R' && i + 1 < text.size() && text[i + 1] == '"') {
      const auto open = text.find('(', i + 2);
      const auto delimiter = ")" + text.substr(i + 2, open == std::string::npos ? 0 : open - i - 2) + "\"";
      const auto end = open == std::string::npos ? std::string::npos : text.find(delimiter, open);
      const auto stop = end == std::string::npos ? text.size() : end + delimiter.size();
      for (; i < stop; ++i) {
        line += text[i] == '\n' ? 1 : 0;
      }
      line_start = false;
    } else if (c == '\'' && i > 0 && text[i - 1] >= '0' && text[i - 1] <= '9') {
      // A digit separator (1'000), not a character literal.
      line_start = false;
      ++i;
    } else if (c == '"' || c == '\'') {
      // A literal ends at its closing quote or before the end of its line, which the next pass counts: an
      // unpaired quote can hide the rest of its own line, never the next one.
      for (++i; i < text.size() && text[i] != c && text[i] != '\n'; ++i) {
        i += text[i] == '\\' && i + 1 < text.size() && text[i + 1] != '\n' ? 1 : 0;
      }
      i += i < text.size() && text[i] == c ? 1 : 0;
      line_start = false;
    } else if (c == '#' && line_start) {
      auto j = text.find_first_not_of(" \t", i + 1);
      if (j != std::string::npos && text.compare(j, 1, "/") == 0) {
        found.push_back({line, "", "a comment inside a directive, which the layout check does not read"});
      } else if (j != std::string::npos && text.compare(j, 7, "include") == 0) {
        j = text.find_first_not_of(" \t", j + 7);
        const auto end = j == std::string::npos ? std::string::npos : text.find('\n', j);
        auto operand = j == std::string::npos ? std::string() :
          text.substr(j, end == std::string::npos ? std::string::npos : end - j);
        const char close = operand.empty() ? 0 : operand[0] == '"' ? '"' : operand[0] == '<' ? '>' : 0;
        const auto last = close == 0 ? std::string::npos : operand.find(close, 1);
        operand = last == std::string::npos ? operand : operand.substr(0, last + 1);
        if (operand.compare(0, 1, "/") == 0) {
          found.push_back({line, "", "a comment inside a directive, which the layout check does not read"});
        } else {
          found.push_back({line, operand, ""});
        }
      }
      line_start = false;
      ++i;
    } else {
      line_start = line_start && (c == ' ' || c == '\t');
      ++i;
    }
  }
  return found;
}

// Cases the lexer must read, run by `check-layout --self-test` (ctest `layout-lexer`): each source and the
// directives it gives, an operand or `!` for a refusal.
int self_test() {
  const std::vector<std::pair<std::string, std::vector<std::string>>> cases{
    {"#include \"a.hpp\"\n#include <b>\n", {"\"a.hpp\"", "<b>"}},
    {"  #  include \"a.hpp\"\n", {"\"a.hpp\""}},
    {"#if 0\n#include \"a.hpp\"\n#endif\n", {"\"a.hpp\""}},
    {"/* see https://x.y\n#include \"a.hpp\"\n*/\n", {}},
    {"// #include \"a.hpp\"\n", {}},
    {"const char* t = R\"x(\n#include \"a.hpp\"\n)x\";\n", {}},
    {"const char* t = u8R\"(\n#include \"a.hpp\"\n)\";\n", {}},
    {"const char* t = \"/*\";\n#include \"a.hpp\"\n", {"\"a.hpp\""}},
    {"inline constexpr int n = 1'000;\n#include \"a.hpp\"\n", {"\"a.hpp\""}},
    {"char c = '\"';\n#include \"a.hpp\"\n", {"\"a.hpp\""}},
    {"const char* s = \"unclosed\n#include \"a.hpp\"\n", {"\"a.hpp\""}},
    {"/* a */ #include \"a.hpp\"\n", {"\"a.hpp\""}},
    {"#define H \"a.hpp\"\n#include H\n", {"H"}},
    {"#inc\\\nlude \"a.hpp\"\n", {"!"}},
    {"// comment \\\n#include \"a.hpp\"\n", {"!", "\"a.hpp\""}},
    {"#/**/include \"a.hpp\"\n", {"!"}},
    {"#include /**/ <string>\n", {"!"}},
    {"#include", {""}},
    {"%:include \"a.hpp\"\n", {"!"}},
    {"\f#include \"a.hpp\"\n", {"!"}},
    {"#\vinclude \"a.hpp\"\n", {"!"}},
    {"int n = 0;\r#include \"a.hpp\"\n", {"!"}},
    {"\xEF\xBB\xBF#include \"a.hpp\"\n", {"!"}},
  };
  int failures = 0;
  for (const auto& [source, expected] : cases) {
    std::vector<std::string> got;
    for (const auto& directive : includes_of(source)) {
      got.push_back(directive.problem_.empty() ? directive.operand_ : "!");
    }
    if (got != expected) {
      ++failures;
      std::cout << "self-test failed on: " << source << '\n';
    }
  }
  std::cout << cases.size() - static_cast<std::size_t>(failures) << " of " << cases.size() << " lexer cases passed\n";
  return failures == 0 ? 0 : 1;
}

// The module of a path relative to include/cmdatl/, or empty for a header at its root.
std::string module_of(const fs::path& relative) {
  const auto first = relative.begin();
  return std::next(first) == relative.end() ? std::string() : first->string();
}

// Whether a path lies inside a directory (both normalized), by whole components.
bool inside(const fs::path& path, const fs::path& dir) {
  const auto relative = path.lexically_relative(dir);
  return !relative.empty() && *relative.begin() != "..";
}

} // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--self-test") {
    return self_test();
  }
  if (argc != 2) {
    std::cerr << "usage: check-layout SOURCE_ROOT | --self-test\n";
    return 2;
  }
  const auto include = fs::absolute(fs::path(argv[1]) / "include").lexically_normal();
  const auto tree = (include / "cmdatl").lexically_normal();
  if (!fs::is_directory(tree)) {
    std::cerr << tree.string() << ": no such directory\n";
    return 2;
  }
  std::vector<std::string> problems;
  std::size_t headers = 0;
  for (const auto& entry : fs::recursive_directory_iterator(tree)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".hpp") {
      continue;
    }
    ++headers;
    const auto path = entry.path().lexically_normal();
    const auto name = path.lexically_relative(tree);
    const auto module = module_of(name);
    const auto rules = ALLOWED.find(module);
    if (rules == ALLOWED.end()) {
      problems.push_back(name.string() + ": not in a module directory (" + (module.empty() ? "the root" : module) + ")");
      continue;
    }
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    for (const auto& directive : includes_of(text.str())) {
      const auto where = name.string() + ":" + std::to_string(directive.line_) + ": ";
      if (!directive.problem_.empty()) {
        problems.push_back(where + directive.problem_);
        continue;
      }
      const auto& operand = directive.operand_;
      if (operand.size() < 2 || (operand.front() != '"' && operand.front() != '<') ||
          operand.back() != (operand.front() == '"' ? '"' : '>')) {
        problems.push_back(where + "an include that is not \"...\" or <...>: " + operand);
        continue;
      }
      const auto target_text = operand.substr(1, operand.size() - 2);
      if (operand.front() == '<') {
        // An external header is allowed; one that resolves into the tree must be written quoted.
        const auto target = (include / target_text).lexically_normal();
        if (inside(target, include) && fs::exists(target)) {
          problems.push_back(where + "a cmdatl header included with <...>; write it \"../module/x.hpp\": " + operand);
        }
        continue;
      }
      const auto target = (path.parent_path() / target_text).lexically_normal();
      if (!inside(target, tree) || !fs::is_regular_file(target)) {
        problems.push_back(where + "no such header under include/cmdatl/: " + operand);
        continue;
      }
      const auto to = module_of(target.lexically_relative(tree));
      if (to != module && rules->second.count(to) == 0) {
        problems.push_back(where + module + " may not include " + (to.empty() ? "the root" : to) + ": " + operand);
      }
    }
  }
  for (const auto& problem : problems) {
    std::cout << problem << '\n';
  }
  if (headers == 0) {
    std::cout << "no headers under " << tree.string() << '\n';
    return 1;
  }
  return problems.empty() ? 0 : 1;
}
