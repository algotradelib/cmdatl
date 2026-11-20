// The built-in brokers' catalog: cmdatl's facts (`core/builtins.hpp`) checked against the library's compile-time list,
// and what the library states about each, read from its traits. Includes nothing of cmdatl's but the facts and the
// `brokers/` headers that read the library, so no inline function a test builds with its hooks is linked from here.

#include <algotradelib/builtin_brokers>
#include <cmdatl/brokers/catalog.hpp>
#include <cmdatl/brokers/derive.hpp>
#include <cmdatl/core/builtins.hpp>

#include <string_view>
#include <vector>

namespace cmdatl {
namespace brokers {
namespace {

/**
 * @brief Whether every row of the facts names a client of the list.
 * @tparam Clients The list's clients.
 * @return Whether it does.
 */
template <typename... Clients>
constexpr bool every_row_listed() {
  for (const auto& facts : builtin::FACTS) {
    if (!((facts.name_ == algotradelib::BrokerTraits<Clients>::name) || ...)) {
      return false;
    }
  }
  return true;
}

/**
 * @brief The entries of the library's list, in the facts' order; the list and the facts checked against each other.
 * @tparam Clients The list's clients.
 * @return The entries.
 */
template <typename... Clients>
std::vector<Entry> entries(algotradelib::type_list<Clients...>) {
  static_assert(
    ((builtin::find(algotradelib::BrokerTraits<Clients>::name) != nullptr) && ...),
    "a client of algotradelib::builtin_brokers has no row in builtin::FACTS (core/builtins.hpp)"
  );
  static_assert(
    every_row_listed<Clients...>(), "a row of builtin::FACTS names no client of algotradelib::builtin_brokers"
  );
  static_assert(sizeof...(Clients) == builtin::FACTS.size(), "builtin::FACTS and algotradelib::builtin_brokers differ");
  const std::vector<Entry> listed{entry<Clients>()...};
  std::vector<Entry> ordered;
  for (const auto& facts : builtin::FACTS) {
    for (const auto& one : listed) {
      if (one.facts_ == &facts) {
        ordered.push_back(one);
      }
    }
  }
  return ordered;
}

} // namespace

const std::vector<Entry>& catalog() {
  static const std::vector<Entry> list = entries(algotradelib::builtin_brokers{});
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
