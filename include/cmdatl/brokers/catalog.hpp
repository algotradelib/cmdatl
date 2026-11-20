#pragma once

// The binary's built-in brokers, in `builtin::FACTS`' order: defined in `src/catalog.cpp` (the static library
// `cmdatl_builtins`), the one translation unit that reads the library's list. No header a driver includes includes this
// one.

#include "./derive.hpp"

#include <string_view>
#include <vector>

namespace cmdatl {
namespace brokers {

/**
 * @brief The built-in brokers, each with cmdatl's facts and what the library states.
 * @return Them, in `builtin::FACTS`' order.
 */
const std::vector<Entry>& catalog();

/**
 * @brief A built-in broker by the user's word, mapped with `builtin::from_word`.
 * @param word The word, such as `toss`.
 * @return Its entry, or null.
 */
const Entry* find_entry(std::string_view word);

} // namespace brokers
} // namespace cmdatl
