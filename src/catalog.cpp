// The built-in brokers' catalog: cmdatl's facts (`core/builtins.hpp`) checked against the library's compile-time list,
// and what the library states about each, read from its traits. Includes nothing of cmdatl's but the facts and
// `brokers/derive.hpp`, so no inline function a test builds with its hooks is linked from here.

#include <algotradelib/builtin_brokers>
#include <cmdatl/brokers/derive.hpp>

#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

namespace cmdatl {
namespace brokers {
namespace {

/**
 * @brief The entries of the library's list, which must name the rows of the facts one for one, in their order (a
 *   renamed or added client fails the build, never a command).
 * @tparam Clients The list's clients.
 * @tparam I Their positions.
 * @return The entries.
 */
template <typename... Clients, std::size_t... I>
std::vector<Entry> entries(algotradelib::type_list<Clients...>, std::index_sequence<I...>) {
  static_assert(
    sizeof...(Clients) == builtin::FACTS.size() &&
      ((builtin::FACTS[I].name_ == algotradelib::BrokerTraits<Clients>::name) && ...),
    "builtin::FACTS (core/builtins.hpp) and algotradelib::builtin_brokers differ"
  );
  return {entry<Clients>()...};
}

} // namespace

const std::vector<Entry>& catalog() {
  static const std::vector<Entry> list =
    entries(algotradelib::builtin_brokers{}, std::make_index_sequence<algotradelib::builtin_brokers::size>{});
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
