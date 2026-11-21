// The built-in brokers' catalog: cmdatl's facts (`core/builtins.hpp`) checked against the library's compile-time list
// (every row names a listed client; a listed client without a row is not offered), and what the library states about
// each, read from its traits. Includes nothing of cmdatl's but the facts and
// `brokers/derive.hpp`, so no inline function a test builds with its hooks is linked from here.

#include <algotradelib/builtin_brokers>
#include <cmdatl/brokers/derive.hpp>

#include <string_view>
#include <vector>

namespace cmdatl {
namespace brokers {

// A renamed or removed client fails the build, never a command.
static_assert(
  lists_every_row<algotradelib::builtin_brokers>(),
  "a row of builtin::FACTS (core/builtins.hpp) names no client in algotradelib::builtin_brokers"
);

const std::vector<Entry>& catalog() {
  static const std::vector<Entry> list = catalog_of<algotradelib::builtin_brokers>();
  return list;
}

const Entry* find_entry(std::string_view word) {
  const auto* facts = builtin::from_word(word);
  for (const auto& one : catalog()) {
    if (facts != nullptr && one.facts_ == facts) {
      return &one;
    }
  }
  return nullptr;
}

} // namespace brokers
} // namespace cmdatl
