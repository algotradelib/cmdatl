#pragma once

// A broker folder's adapter for tests: a data vendor on the library's DataOnlyTraits, serving a fixed daily history
// (`history` bars from 2024-01-01, prices to the cent) of any symbol, the last `count` of them. A login whose token
// starts with `fail` is answered with an HTTP 401 whose text holds the token, as a vendor putting its token in a URL
// would; one starting with `empty` is answered with no bars. Its options (broker.toml), both required: `exchange`, a
// string, and `history`, an integer; it has no default of its own.

#include <algotradelib/broker>

#include <boost/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace fixture_vendor {

/** @brief The vendor's name. */
struct Tag {
  /** @brief The name. */
  static constexpr std::string_view name = "fixture-vendor";
};

/** @brief The client: the token and the history's length. */
struct Client {
  /** @brief The token. */
  std::string token_;
  /** @brief How many bars the history holds. */
  int history_{0};
};

/**
 * @brief The fixed history's bar `k`.
 * @param k The bar, from 0.
 * @return The bar.
 */
inline algotradelib::Bar bar_at(int k) {
  namespace atl = algotradelib;
  const auto price = [](double value) {
    char text[32];
    std::snprintf(text, sizeof text, "%.2f", value);
    return atl::Decimal64::parse(text);
  };
  const double close = 100.0 + 8.0 * std::sin(k / 4.0) + 0.05 * k;
  atl::Bar bar;
  // 2024-01-01 is 19723 days after 1970-01-01.
  bar.timestamp_ = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * (19723 + k));
  bar.open_ = price(close - 0.5);
  bar.high_ = price(close + 1.0);
  bar.low_ = price(close - 1.5);
  bar.close_ = price(close);
  bar.volume_ = atl::Decimal64::parse("1000");
  return bar;
}

} // namespace fixture_vendor

/** @brief The fixture vendor's traits: daily bars only. */
template <>
struct algotradelib::BrokerTraits<fixture_vendor::Client> : algotradelib::DataOnlyTraits<fixture_vendor::Tag> {
  /** @brief The client. */
  using Client = fixture_vendor::Client;
  /** @brief No configuration. */
  using Config = fixture_vendor::Client;
  /** @brief Its name. */
  static constexpr std::string_view name = BrokerTag::name;
  /** @brief Blocking. */
  static constexpr bool blocking = true;

  /**
   * @brief Daily bars only.
   * @param interval The interval.
   * @return Whether it is daily.
   */
  static constexpr bool supports_interval(BarInterval interval) {
    return interval == BarInterval::ONE_DAY;
  }

  /**
   * @brief Refuses another interval.
   * @param query The query.
   * @return The refusal, or none.
   */
  static std::optional<BrokerError> validate_bars(const BarQuery& query) {
    if (supports_interval(query.interval_)) {
      return std::nullopt;
    }
    return data_only::refusal();
  }

  /**
   * @brief The fixed history's last `count` bars, or a 401 for a failing token.
   * @param client The client.
   * @param query The query.
   * @return The bars.
   */
  static Result<BarSeries> bars(Client& client, BarQuery query) {
    if (client.token_.rfind("fail", 0) == 0) {
      BrokerError error;
      error.category_ = ErrorCategory::HTTP;
      error.http_status_ = 401;
      error.broker_code_ = "E401";
      error.message_ = "GET /bars?api_token=" + client.token_ + " answered 401";
      return Result<BarSeries>(error);
    }
    BarSeries series;
    if (client.token_.rfind("empty", 0) == 0) {
      series.instrument_ = query.instrument_;
      series.interval_ = query.interval_;
      series.adjusted_ = query.adjusted_;
      return Result<BarSeries>(series);
    }
    series.instrument_ = query.instrument_;
    series.interval_ = query.interval_;
    series.stamp_ = BarStamp::OPEN;
    series.adjusted_ = query.adjusted_;
    series.currency_ = Currency(query.instrument_.country_ == Country::KR ? "KRW" : "USD");
    const int total = client.history_;
    for (int k = std::max(0, total - query.count_); k < total; ++k) {
      series.bars_.push_back(fixture_vendor::bar_at(k));
    }
    return Result<BarSeries>(series);
  }
};

namespace cmdatl_adapter {

/** @brief The client. */
using Client = fixture_vendor::Client;

/** @brief What `connect` returns. */
struct Connection {
  /** @brief The client. */
  std::shared_ptr<Client> client_;
  /** @brief No account. */
  algotradelib::data_only::NoAccount account_;
  /** @brief None. */
  std::string account_id_;
  /** @brief Never demo. */
  bool demo_{false};
};

/**
 * @brief Connects with the stored login, `{"api_token": ...}`, and the options, `{"exchange": ..., "history": ...}`.
 * @param login The login.
 * @param options The options.
 * @return The connection.
 */
inline Connection connect(const std::string& login, const std::string& options) {
  const auto given = boost::json::parse(options).as_object();
  (void)given.at("exchange").as_string();
  Connection connection;
  connection.client_ = std::make_shared<Client>();
  connection.client_->token_ = std::string(boost::json::parse(login).as_object().at("api_token").as_string());
  connection.client_->history_ = static_cast<int>(given.at("history").as_int64());
  return connection;
}

} // namespace cmdatl_adapter
