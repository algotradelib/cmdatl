#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace cmdatl {

/** @brief The clock of the mock market and of every instant cmdatl writes. */
using Clock = std::chrono::system_clock;

/** @brief Korea's offset from UTC; KRX has no daylight saving time. */
inline constexpr std::chrono::hours KST_OFFSET{9};

/** @brief The length of a KRX regular session, 09:00 to 15:30 KST. */
inline constexpr std::chrono::minutes SESSION_LENGTH{6 * 60 + 30};

/**
 * @brief The days from 1970-01-01 to a Gregorian date (Howard Hinnant's `days_from_civil`).
 * @param year The year.
 * @param month The month, 1 to 12.
 * @param day The day, 1 to 31.
 * @return The days; negative before 1970.
 */
inline constexpr std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept {
  year -= month <= 2 ? 1 : 0;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const auto yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

/** @brief A Gregorian date. */
struct Civil {
  /** @brief The year. */
  std::int64_t year_;
  /** @brief The month, 1 to 12. */
  unsigned month_;
  /** @brief The day, 1 to 31. */
  unsigned day_;
};

/**
 * @brief The Gregorian date some days from 1970-01-01 (Howard Hinnant's `civil_from_days`).
 * @param days The days.
 * @return The date.
 */
inline constexpr Civil civil_from_days(std::int64_t days) noexcept {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const auto doe = static_cast<unsigned>(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned day = doy - (153 * mp + 2) / 5 + 1;
  const unsigned month = mp < 10 ? mp + 3 : mp - 9;
  return {static_cast<std::int64_t>(yoe) + era * 400 + (month <= 2 ? 1 : 0), month, day};
}

/** @brief The first date of the supported interval, 1990-01-01, as days from 1970-01-01. */
inline constexpr std::int64_t FIRST_DAY = days_from_civil(1990, 1, 1);
/** @brief The last date of the supported interval, 2100-12-31, as days from 1970-01-01. */
inline constexpr std::int64_t LAST_DAY = days_from_civil(2100, 12, 31);

/**
 * @brief The 00:00 UTC of a date, which is 09:00 KST: the open of that date's session.
 * @param days The date, as days from 1970-01-01, inside the supported interval.
 * @return The instant.
 */
inline Clock::time_point open_of(std::int64_t days) {
  return Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::hours(24 * days)));
}

/** @brief The first instant of the supported interval: 1990-01-01T00:00 KST. */
inline Clock::time_point first_instant() {
  return open_of(FIRST_DAY) - KST_OFFSET;
}

/** @brief The end of the supported interval: 2101-01-01T00:00 KST, itself outside it. */
inline Clock::time_point end_instant() {
  return open_of(LAST_DAY + 1) - KST_OFFSET;
}

/**
 * @brief Whether an instant is inside the supported interval, 1990-01-01 to 2100-12-31 KST.
 * @param at The instant.
 * @return Whether it is.
 */
inline bool in_interval(Clock::time_point at) {
  return at >= first_instant() && at < end_instant();
}

/**
 * @brief The UTC date an instant falls on, as days from 1970-01-01 (rounded down, before 1970 too).
 * @param at The instant.
 * @return The days.
 */
inline std::int64_t utc_day(Clock::time_point at) {
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(at.time_since_epoch()).count();
  return seconds >= 0 ? seconds / 86400 : -((-seconds + 86399) / 86400);
}

/**
 * @brief The 00:00 UTC that starts an instant's UTC date: the open of the session on that date.
 * @param at The instant.
 * @return The start.
 */
inline Clock::time_point day_start(Clock::time_point at) {
  return open_of(utc_day(at));
}

/**
 * @brief The KST date an instant falls on, as days from 1970-01-01.
 * @param at The instant.
 * @return The days.
 */
inline std::int64_t kst_day(Clock::time_point at) {
  return utc_day(at + KST_OFFSET);
}

/**
 * @brief Reads exactly `count` decimal digits.
 * @param text The text.
 * @param at Where they start.
 * @param count How many.
 * @return Their value, or nothing when one is not a digit.
 */
inline std::optional<int> digits_at(std::string_view text, std::size_t at, std::size_t count) {
  if (at + count > text.size()) {
    return std::nullopt;
  }
  int value = 0;
  for (std::size_t i = at; i < at + count; ++i) {
    if (text[i] < '0' || text[i] > '9') {
      return std::nullopt;
    }
    value = value * 10 + (text[i] - '0');
  }
  return value;
}

/**
 * @brief Reads a date written exactly `YYYY-MM-DD`, a real date inside the supported interval.
 * @param text The text.
 * @return The date as days from 1970-01-01, or nothing.
 */
inline std::optional<std::int64_t> parse_date(std::string_view text) {
  if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
    return std::nullopt;
  }
  const auto year = digits_at(text, 0, 4);
  const auto month = digits_at(text, 5, 2);
  const auto day = digits_at(text, 8, 2);
  if (!year || !month || !day || *month < 1 || *month > 12 || *day < 1) {
    return std::nullopt;
  }
  static constexpr unsigned lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (*year % 4 == 0 && *year % 100 != 0) || *year % 400 == 0;
  const unsigned length = lengths[*month - 1] + (*month == 2 && leap ? 1 : 0);
  if (static_cast<unsigned>(*day) > length) {
    return std::nullopt;
  }
  const auto days = days_from_civil(*year, static_cast<unsigned>(*month), static_cast<unsigned>(*day));
  if (days < FIRST_DAY || days > LAST_DAY) {
    return std::nullopt;
  }
  return days;
}

/**
 * @brief Reads a KST instant written exactly `YYYY-MM-DDTHH:MM`, inside the supported interval.
 * @param text The text.
 * @return The instant, or nothing.
 */
inline std::optional<Clock::time_point> parse_kst(std::string_view text) {
  if (text.size() != 16 || text[10] != 'T' || text[13] != ':') {
    return std::nullopt;
  }
  const auto days = parse_date(text.substr(0, 10));
  const auto hours = digits_at(text, 11, 2);
  const auto minutes = digits_at(text, 14, 2);
  if (!days || !hours || !minutes || *hours > 23 || *minutes > 59) {
    return std::nullopt;
  }
  return open_of(*days) + std::chrono::hours(*hours) + std::chrono::minutes(*minutes) - KST_OFFSET;
}

/**
 * @brief A date as `YYYY-MM-DD`.
 * @param days The date, as days from 1970-01-01.
 * @return The text.
 */
inline std::string date_text(std::int64_t days) {
  const auto civil = civil_from_days(days);
  char buffer[32];
  std::snprintf(
    buffer, sizeof(buffer), "%04lld-%02u-%02u", static_cast<long long>(civil.year_), civil.month_, civil.day_
  );
  return buffer;
}

/**
 * @brief An instant in KST with its offset, `YYYY-MM-DDTHH:MM:SS+09:00`, whole seconds.
 * @param at The instant.
 * @return The text.
 */
inline std::string kst_iso(Clock::time_point at) {
  const auto local = at + KST_OFFSET;
  const auto days = utc_day(local);
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(local - open_of(days)).count();
  char buffer[48];
  std::snprintf(
    buffer, sizeof(buffer), "%sT%02lld:%02lld:%02lld+09:00", date_text(days).c_str(),
    static_cast<long long>(seconds / 3600), static_cast<long long>(seconds / 60 % 60),
    static_cast<long long>(seconds % 60)
  );
  return buffer;
}

/**
 * @brief An instant as the nanoseconds since 1970 cmdatl writes.
 * @param at The instant, inside the supported interval.
 * @return The nanoseconds.
 */
inline std::int64_t nanos(Clock::time_point at) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(at.time_since_epoch()).count();
}

/**
 * @brief The instant cmdatl wrote, when it is inside the supported interval and has no finer part than the
 *   clock keeps (so writing it again gives the same number).
 * @param value The nanoseconds since 1970.
 * @return The instant, or nothing.
 */
inline std::optional<Clock::time_point> from_nanos(std::int64_t value) {
  if (value < nanos(first_instant()) || value >= nanos(end_instant())) {
    return std::nullopt;
  }
  const auto at = Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(value)));
  if (nanos(at) != value) {
    return std::nullopt;
  }
  return at;
}

} // namespace cmdatl
