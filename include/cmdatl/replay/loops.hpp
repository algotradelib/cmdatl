#pragma once

#include "./evaluate.hpp"

#include <algotradelib/analytics>

#include <numeric>
#include <random>

/**
 * @file
 * @brief The evaluation methods over the many-replays core: `windows` (one parameter set over rolling or expanding
 *   windows), `walk-forward` (a set chosen on each training window by `evaluate.objective`, replayed on the next test
 *   window, the tests chained by the library) and `bootstrap` (block-bootstrap paths of the closes; path 0 the history).
 *
 * The orders: window `k` is replay `k`; walk-forward's training replay of fold `k` and set `g` of `S` is `k * S + g`,
 * then one test per fold with a choice, numbered on from `F * S` in fold order; bootstrap path `p` is replay `p`.
 *
 * A length is a whole number of periods (the market dates holding a bar) or a calendar span (`10d`, `2w`, `6mo`,
 * `3y`); every boundary of a span is the anchor plus a sum of lengths, months first (the day clamped to the month's
 * last), never one boundary from another.
 */

namespace cmdatl {
namespace replay {

/** @brief A length as written: a whole number of periods, or a calendar span of months and days. */
struct Length {
  /** @brief The text, as written. */
  std::string text_;
  /** @brief The periods, or none for a span. */
  std::optional<std::size_t> periods_;
  /** @brief A span's months and days (one of them zero). */
  long long months_{0}, days_{0};
};

/**
 * @brief Reads a length: digits (periods, `least` to 1e9), or 1 to 99999 and a unit, `d`, `w`, `mo` or `y`.
 * @param key The setting, for errors.
 * @param text The text.
 * @param least The fewest periods.
 * @return The length.
 */
inline Length length_of(const std::string& key, const std::string& text, std::size_t least) {
  const auto digits = std::min(text.find_first_not_of("0123456789"), text.size());
  const auto unit = text.substr(digits);
  const bool span = unit == "d" || unit == "w" || unit == "mo" || unit == "y";
  const auto n = digits == 0 || digits > (span ? 5 : 10) ? -1 : std::stoll(text.substr(0, digits));
  if (n < 0 || (!span && !unit.empty()) || (text[0] == '0' && (span || digits > 1)) || (!span && n > 1000000000)) {
    throw std::invalid_argument(key + " is a whole number of periods or a span (10d, 2w, 6mo, 3y), not '" + text + "'");
  }
  if (!span && n < static_cast<long long>(least)) {
    throw std::invalid_argument(key + " is " + std::to_string(least) + " periods or more, not " + text);
  }
  return {
    text, span ? std::nullopt : std::optional<std::size_t>(static_cast<std::size_t>(n)),
    unit == "mo" ?
      n :
      unit == "y" ?
      12 * n :
      0,
    unit == "d" ?
      n :
      unit == "w" ?
      7 * n :
      0
  };
}

/**
 * @brief A date plus months (the day clamped to the month's last), then days.
 * @param date The date, yyyymmdd.
 * @param months The months, 0 or more.
 * @param days The days, 0 or more.
 * @return The date, yyyymmdd; refused past the year 9999 or this platform's clock.
 */
inline int plus(int date, long long months, long long days) {
  const long long m = date / 100 % 100 - 1 + months;
  const long long y = date / 10000 + m / 12;
  if (y > 9999 || days > 4000000) {
    throw Refused("a window's boundary falls after the year 9999", "give shorter lengths");
  }
  auto ymd = static_cast<int>(y * 10000 + (m % 12 + 1) * 100 + date % 100);
  while (ymd % 100 > 28 && !date_exists(ymd)) {
    --ymd;
  }
  try {
    return date_of(iso(epoch_plus(day_seconds(ymd) + days * 86400, "a window's boundary")).substr(0, 10), "");
  } catch (const std::invalid_argument& e) {
    throw Refused(e.what(), "give shorter lengths");
  }
}

/**
 * @brief Whether bars of an interval are daily or coarser.
 * @param interval The interval.
 * @return Whether they are.
 */
inline bool daily(atl::BarInterval interval) {
  return interval == atl::BarInterval::ONE_DAY || interval == atl::BarInterval::ONE_WEEK ||
    interval == atl::BarInterval::ONE_MONTH || interval == atl::BarInterval::ONE_YEAR;
}

/** @brief The period index: the market dates holding a bar, each with its first event. */
struct Periods {
  /** @brief Each period's date, yyyymmdd. */
  std::vector<int> dates_;
  /** @brief Each period's first event, then the event count. */
  std::vector<std::size_t> first_;

  /**
   * @brief The first period dated at or after a date.
   * @param date The date.
   * @return Its position, or the count when none is.
   */
  std::size_t pos(int date) const {
    return static_cast<std::size_t>(std::lower_bound(dates_.begin(), dates_.end(), date) - dates_.begin());
  }
};

/**
 * @brief The periods of a feed: a daily or coarser bar dated as `bars.open_point` dates it (`market_noon`), an intraday
 *   bar by the market-local date of its start.
 * @param events The events, in time order.
 * @param market The market preset.
 * @return The periods.
 */
inline Periods periods_of(const std::vector<atl::strategy::BarEvent>& events, const std::string& market) {
  const auto session = session_of(market);
  Periods p;
  for (std::size_t i = 0; i < events.size(); ++i) {
    const auto& e = events[i];
    const auto at = daily(e.interval_) ?
      market_noon(e.bar_.timestamp_, session) :
      atl::strategy::bar_period_start(e, session).value_or(e.bar_.timestamp_);
    const int date = date_of(iso(at + session.utc_offset_).substr(0, 10), "a bar's date");
    if (!p.dates_.empty() && date < p.dates_.back()) {
      throw std::invalid_argument("the bars' market dates go back at " + iso(e.bar_.timestamp_));
    }
    if (p.dates_.empty() || date != p.dates_.back()) {
      p.dates_.push_back(date);
      p.first_.push_back(i);
    }
  }
  p.first_.push_back(events.size());
  return p;
}

/**
 * @brief Windows of span lengths: window `k` trades `[A + lead + j step, A + lead + length + k step)` and its history
 *   starts at `A + g step` (or `back` periods before its start), `g = k` when rolling and `j = k` when its start moves,
 *   else 0; kept while its end is at most `limit`.
 * @param p The periods.
 * @param anchor The anchor `A`, yyyymmdd.
 * @param limit The latest end, yyyymmdd.
 * @param lead The lead.
 * @param length The length.
 * @param step The step.
 * @param rolling Whether the history start moves.
 * @param moves Whether the traded start moves.
 * @param back The history in periods, or none for `A + g step`.
 * @return Each window as a fold: `train_` traded, `train_warmup_` where its history starts.
 */
inline std::vector<atl::backtest::Fold> span_folds(
  const Periods& p, int anchor, int limit, const Length& lead, const Length& length, const Length& step, bool rolling,
  bool moves, std::optional<std::size_t> back
) {
  std::vector<atl::backtest::Fold> out;
  for (long long k = 0;; ++k) {
    const auto at = [&](long long a, long long b, long long c) {
      return plus(
        anchor, a * lead.months_ + b * length.months_ + c * step.months_,
        a * lead.days_ + b * length.days_ + c * step.days_
      );
    };
    const int end = at(1, 1, k);
    if (end > limit) {
      return out;
    }
    const auto begin = p.pos(at(1, 0, moves ? k : 0));
    const auto from = back ? begin - std::min(begin, *back) : p.pos(at(0, 0, rolling ? k : 0));
    out.push_back({out.size(), {begin, p.pos(end)}, {}, from, from});
    check_count(out.size());
  }
}

/**
 * @brief The warm-up a program states with a parameter set: `warmup_bars()` after `set_params`.
 * @tparam Strategy A strategy generated by `atlc`.
 * @param s The settings.
 * @param prepared The data.
 * @param params The set's parameters over the settings'.
 * @return The bars, or none when the bounds refuse the set.
 */
template <typename Strategy>
std::optional<std::size_t> warmup_of(
  const Settings& s, const Prepared& prepared, const std::vector<std::pair<std::string, std::string>>& params
) {
  Settings with = s;
  for (const auto& param : params) {
    with.add("param." + param.first, param.second);
  }
  Strategy strategy(prepared.ids_);
  try {
    set_params(strategy, with);
  } catch (const std::exception&) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(strategy.warmup_bars());
}

/**
 * @brief The distinct bar instants of periods `[from, to)` (an open point is not a bar).
 * @param prepared The data.
 * @param p The periods.
 * @param from The first period.
 * @param to One past the last.
 * @return The count.
 */
inline std::size_t instants(const Prepared& prepared, const Periods& p, std::size_t from, std::size_t to) {
  if (prepared.open_point_) {
    return to - from;
  }
  std::size_t count = 0;
  for (auto i = p.first_[from]; i < p.first_[to]; ++i) {
    count += i == p.first_[from] || prepared.events_[i].bar_.timestamp_ != prepared.events_[i - 1].bar_.timestamp_;
  }
  return count;
}

/**
 * @brief A replay of periods `[from, end)` trading from `begin`: its window and its task.
 * @param prepared The data.
 * @param p The periods.
 * @param index The replay's index.
 * @param params Its parameters.
 * @param fold Its fold.
 * @param role `window`, `train` or `test`.
 * @param from Where its history starts.
 * @param begin Where it trades from.
 * @param end One past its last period.
 * @return The task; with history, `trade_from_` is the smallest period end of period `begin`.
 */
inline Task window_task(
  const std::shared_ptr<const Prepared>& prepared, const Periods& p, std::size_t index,
  std::vector<std::pair<std::string, std::string>> params, std::size_t fold, const char* role, std::size_t from,
  std::size_t begin, std::size_t end
) {
  const auto& events = prepared->events_;
  const Window w{
    fold, role, std::nullopt, events[p.first_[begin]].bar_.timestamp_, events[p.first_[end] - 1].bar_.timestamp_,
    end - begin
  };
  Task task{Replay{index, std::move(params), w, std::nullopt}, prepared, nullptr, p.first_[from], p.first_[end], {}};
  if (from < begin) {
    task.replay_.window_->warm_from_ = events[p.first_[from]].bar_.timestamp_;
    task.trade_from_ = events[p.first_[begin]].period_end_;
    for (auto i = p.first_[begin]; i < p.first_[begin + 1]; ++i) {
      task.trade_from_ = std::min(*task.trade_from_, events[i].period_end_);
    }
  }
  return task;
}

/**
 * @brief A quantile, Hyndman and Fan's type 7 (numpy's default): `h = (m - 1) p`, `j = floor(h)`, then
 *   `x[j] + (h - j)(x[j + 1] - x[j])`, or `x[j]` at the last.
 * @param sorted The values, in increasing order.
 * @param p The probability, 0 to 1.
 * @return The quantile, NaN without a value.
 */
inline double quantile(const std::vector<double>& sorted, double p) {
  const auto m = sorted.size();
  const double h = (static_cast<double>(m) - 1) * p;
  const auto j = static_cast<std::size_t>(std::floor(std::max(h, 0.0)));
  return m == 0 ?
    std::nan("") :
    j + 1 < m ?
    sorted[j] + (h - static_cast<double>(j)) * (sorted[j + 1] - sorted[j]) :
    sorted[j];
}

/**
 * @brief The statistics `windows.summary` names of a figure: `mean`, and quantiles (`quantile`), `min`, `median` and
 *   `max` being `q00`, `q50` and `q100`.
 * @param values The finite values.
 * @param nulls How many were not.
 * @param statistics The statistics, checked: `mean`, `min`, `max`, `median`, or `q` and a percent in two digits or 100.
 * @return `{"defined", "null"}`, then each statistic in order, null without a value.
 */
inline std::string spread_json(
  std::vector<double> values, std::size_t nulls, const std::vector<std::string>& statistics
) {
  std::sort(values.begin(), values.end());
  const auto m = values.size();
  const auto mean = m == 0 ? std::nan("") : std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(m);
  std::string out = "{\"defined\":" + std::to_string(m) + ",\"null\":" + std::to_string(nulls);
  for (const auto& name : statistics) {
    const auto percent = name == "min" ? "00" : name == "median" ? "50" : name == "max" ? "100" : name.substr(1);
    out += "," + quoted(name) + ":" + number(name == "mean" ? mean : quantile(values, std::stod(percent) / 100));
  }
  return out + "}";
}

/**
 * @brief A figure of a replay.
 * @param figures Its figures.
 * @param name The figure.
 * @return Its value, or NaN without one.
 */
inline double figure(const std::vector<std::pair<std::string, double>>& figures, const std::string& name) {
  const auto found = std::find_if(figures.begin(), figures.end(), [&name](const auto& f) {
    return f.first == name;
  });
  return found == figures.end() ? std::nan("") : found->second;
}

/**
 * @brief An object of the figures a spread or a path's history gives, the measures under `measures`.
 * @param measures The measures' names.
 * @param of Each figure's JSON value, from its name (`measures.NAME` for a measure).
 * @return The object.
 */
template <typename Of>
std::string by_figure(const std::vector<std::string>& measures, Of of) {
  std::string out = "{";
  for (
    const char* name :
    {"total_return", "sharpe_annualized", "sortino_per_period", "max_drawdown", "turnover", "executions"}
  ) {
    out += (out.size() > 1 ? "," : "") + quoted(name) + ":" + of(name);
  }
  for (std::size_t k = 0; k < measures.size(); ++k) {
    out += (k == 0 ? ",\"measures\":{" : ",") + quoted(measures[k]) + ":" + of("measures." + measures[k]);
  }
  return out + (measures.empty() ? "}" : "}}");
}

/** @brief The lines printed and what the reply counts of them. */
struct Tally {
  /** @brief Lines, failed replays, halted replays. */
  std::size_t replays_{0}, failed_{0}, halted_{0};
  /** @brief Each set's refusal by its bounds. */
  std::vector<bool> refused_;
  /** @brief The figures of each replay summarized: a report that did not halt. */
  std::vector<std::vector<std::pair<std::string, double>>> spread_;

  /**
   * @brief Prints a finished replay's line and counts it.
   * @param done The replay.
   * @param set Its parameter set.
   * @param spread Whether its figures join the summary.
   */
  template <typename Done>
  void take(const Done& done, std::size_t set, bool spread) {
    std::cout << done.line_ << '\n';
    std::cout.flush();
    ++replays_;
    if (done.failed_ == ReplayStage::PARAMS) {
      refused_[set] = true;
    } else if (done.failed_) {
      ++failed_;
    } else if (done.halted_) {
      ++halted_;
    } else if (spread) {
      spread_.push_back(done.figures_);
    }
  }

  /**
   * @brief The summary fields every method gives, `best` null.
   * @param method The method.
   * @param jobs The jobs.
   * @param paths `paths`' JSON value.
   * @param objective The objective, or none.
   * @param direction The direction.
   * @return The fields.
   */
  std::string head(
    const std::string& method, std::size_t jobs, const std::string& paths, const std::optional<std::string>& objective,
    const std::string& direction
  ) const {
    const auto invalid = static_cast<std::size_t>(std::count(refused_.begin(), refused_.end(), true));
    return "\"method\":" + quoted(method) + ",\"jobs\":" + std::to_string(jobs) +
      ",\"param_sets\":" + std::to_string(refused_.size()) + ",\"invalid\":" + std::to_string(invalid) + ",\"tried\":" +
      std::to_string(refused_.size() - invalid) + ",\"replays\":" + std::to_string(replays_) + ",\"paths\":" + paths +
      ",\"failed\":" + std::to_string(failed_) + ",\"objective\":" + (objective ? quoted(*objective) : "null") +
      ",\"direction\":" + (objective ? quoted(direction) : "null") + ",\"best\":null";
  }
};

/**
 * @brief Runs a list of tasks (`run_replays`), each after `build` completes it; a `std::exception` from `build` is that
 *   replay's failure.
 *
 * The tasks' indices are consecutive, from any first one.
 * @param s The settings.
 * @param tasks The tasks, in index order.
 * @param jobs The jobs.
 * @param fills_of Gives the fill model.
 * @param measures The measures.
 * @param keep Keeps what the caller needs of each result.
 * @param build Completes a task on its worker.
 * @param sink Takes each finished replay in index order.
 * @param hooks The runner's hooks.
 */
template <typename Strategy, typename Fills, typename Measures, typename Keep, typename Build, typename Sink>
void run_list(
  const Settings& s, std::vector<Task> tasks, std::size_t jobs, Fills& fills_of, const Measures& measures, Keep keep,
  Build build, Sink sink, const RunnerHooks& hooks
) {
  // The runner prints from index 0: a list numbered on from another (walk-forward's tests) is run from 0.
  const auto offset = tasks.empty() ? 0 : tasks.front().replay_.index_;
  struct Source {
    std::vector<Task>& tasks_;
    std::atomic<std::size_t> next_{0};
    std::optional<Task> next(const std::atomic<bool>&) {
      const auto at = next_.fetch_add(1);
      if (at >= tasks_.size()) {
        return std::nullopt;
      }
      tasks_[at].replay_.index_ = at;
      return std::move(tasks_[at]);
    }
  } source{tasks};
  auto work = [&](Task task) {
    task.replay_.index_ += offset;
    try {
      build(task);
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const std::exception& e) {
      decltype(finish_one<Strategy>(s, std::move(task), fills_of, measures, keep)) done;
      done.replay_ = task.replay_;
      done.failed_ = ReplayStage::REPLAY;
      done.line_ = line_of(task.replay_, nullptr, std::make_pair(ReplayStage::REPLAY, std::string(e.what())));
      return done;
    }
    return finish_one<Strategy>(s, std::move(task), fills_of, measures, keep);
  };
  run_replays(source, jobs, work, sink, hooks);
}

/** @brief Completes nothing. */
struct BuildNothing {
  /** @brief Does nothing. */
  void operator()(Task&) const {}
};

/**
 * @brief Runs `windows`, `walk-forward` or `bootstrap` and prints its lines, then its summary line.
 * @tparam Strategy A strategy generated by `atlc`.
 * @param s The settings.
 * @param method The method.
 * @param fills_of Gives the fill model.
 * @param measures The measures.
 * @param jobs The jobs.
 * @param objective The objective, or none.
 * @param direction The direction.
 * @param hooks The runner's hooks.
 * @return 0 after the summary.
 */
template <typename Strategy, typename Fills, typename Measures>
int loops_main(
  const Settings& s, const std::string& method, Fills& fills_of, const Measures& measures, std::size_t jobs,
  const std::optional<std::string>& objective, const std::string& direction, const RunnerHooks& hooks
) {
  namespace bt = atl::backtest;
  const auto grid = grid_of(s);
  if ((method != "walk-forward" && !grid.empty()) || s.get_or("bars.source", "") == "stdin") {
    throw std::invalid_argument(
      method + " replays loaded bars, one parameter set unless walk-forward: a grid and stdin paths are grid's"
    );
  }
  const auto prepared = std::make_shared<const Prepared>(prepare<Strategy>(s));
  const auto& events = prepared->events_;
  const auto p = periods_of(events, prepared->market_);
  const auto n = p.dates_.size();
  if (n == 0) {
    throw Refused("the history holds no bar", "check replay.from and replay.to");
  }
  std::vector<std::string> names;
  for (const auto& measure : measures) {
    names.emplace_back(measure.first);
  }
  const auto anchor = s.get("replay.from") ? parse_date(*s.get("replay.from")) : p.dates_.front();
  const auto limit = plus(s.get("replay.to") ? parse_date(*s.get("replay.to")) : p.dates_.back(), 0, 1);
  const auto echo = [](const Length& l) {
    return l.periods_ ? std::to_string(*l.periods_) : quoted(l.text_);
  };
  const auto uncovered = [&p, n](std::size_t end) {
    return end < n ? quoted(iso(day_of(p.dates_[end])).substr(0, 10)) : std::string("null");
  };
  Tally tally;
  std::ostringstream o;
  const auto statistics = list_of(s.get_or("windows.summary", ""));
  if (method == "windows") {
    for (auto item = statistics.begin(); item != statistics.end(); ++item) {
      const auto& t = *item;
      const bool known = t == "mean" || t == "min" || t == "max" || t == "median" || t == "q100" ||
        (t.size() == 3 && t[0] == 'q' && t.find_first_not_of("0123456789", 1) == std::string::npos);
      if (!known || std::find(statistics.begin(), item, t) != item) {
        throw Refused(
          known ? "windows.summary names '" + t + "' twice" : "windows.summary names no statistic: '" + t + "'",
          "name each of mean, min, max, median, q00 to q100 at most once"
        );
      }
    }
    const bool rolling = s.get_or("windows.scheme", "rolling") == "rolling";
    const auto length = length_of("windows.length", s.need("windows.length"), 2);
    const auto step = length_of("windows.step", s.get_or("windows.step", length.text_), 1);
    const auto need = warmup_of<Strategy>(s, *prepared, {});
    const bool intraday = std::any_of(events.begin(), events.end(), [](const auto& e) {
      return !daily(e.interval_);
    });
    if (!s.get("windows.warmup") && intraday) {
      throw Refused("on intraday bars the warm-up is yours to give", "give windows.warmup");
    }
    const auto warm = length_of("windows.warmup", s.get_or("windows.warmup", std::to_string(need.value_or(0))), 0);
    if (length.periods_.has_value() != step.periods_.has_value() || (length.periods_ && !warm.periods_)) {
      throw std::invalid_argument("give windows.length and windows.step (and a span windows.warmup) alike");
    }
    std::vector<bt::Fold> folds;
    if (length.periods_ && *warm.periods_ + *length.periods_ <= n) {
      const bt::FoldLengths lengths{*length.periods_, 0, *step.periods_, *warm.periods_};
      folds = rolling ? bt::folds<bt::Rolling>(n, lengths) : bt::folds<bt::Expanding>(n, lengths);
    } else if (!length.periods_ && (!warm.periods_ || *warm.periods_ < n)) {
      const auto a = warm.periods_ ? p.dates_[*warm.periods_] : anchor;
      folds = span_folds(p, a, limit, warm.periods_ ? Length{} : warm, length, step, rolling, rolling, warm.periods_);
    }
    if (folds.empty()) {
      throw Refused("no window fits the history's " + std::to_string(n) + " periods", "give a shorter windows.length");
    }
    check_count(folds.size());
    std::vector<Task> tasks;
    for (const auto& f : folds) {
      const auto have = instants(*prepared, p, f.train_warmup_, f.train_.begin_);
      const auto k = std::to_string(f.index_);
      if (f.train_.end_ - f.train_.begin_ < 2) {
        throw Refused("window " + k + " holds fewer than 2 periods", "give a longer windows.length");
      }
      if (need && have < *need && (!warm.periods_ || *warm.periods_ > 0)) {
        throw Refused(
          "window " + k + "'s warm-up holds " + std::to_string(have) + " bars, and the program needs " +
            std::to_string(*need),
          "give a longer windows.warmup"
        );
      }
      tasks.push_back(
        window_task(prepared, p, f.index_, {}, f.index_, "window", f.train_warmup_, f.train_.begin_, f.train_.end_)
      );
    }
    tally.refused_.assign(1, false);
    run_list<Strategy>(
      s, std::move(tasks), jobs, fills_of, measures,
      [](const auto&, const auto&) {
        return KeepNothing{};
      },
      BuildNothing{},
      [&tally](auto&& done) {
        tally.take(done, 0, true);
      },
      hooks
    );
    o << tally.head(method, jobs, "null", objective, direction)
      << ",\"scheme\":" << quoted(rolling ? "rolling" : "expanding") << ",\"length\":" << echo(length)
      << ",\"step\":" << echo(step) << ",\"warmup\":" << echo(warm) << ",\"windows\":" << folds.size()
      << ",\"uncovered_from\":" << uncovered(folds.back().train_.end_);
  }
  if (method == "walk-forward") {
    const bool rolling = s.get_or("walk_forward.scheme", "rolling") == "rolling";
    const auto train = length_of("walk_forward.train", s.need("walk_forward.train"), 2);
    const auto test = length_of("walk_forward.test", s.need("walk_forward.test"), 2);
    if (train.periods_.has_value() != test.periods_.has_value()) {
      throw std::invalid_argument("give walk_forward.train and walk_forward.test both as periods or both as spans");
    }
    if (!objective) {
      throw std::invalid_argument("walk-forward chooses by evaluate.objective");
    }
    const auto sets = set_count(grid);
    if (sets < 2) {
      throw std::invalid_argument("nothing to choose: give grid.NAME, or use windows");
    }
    std::vector<bt::Fold> folds;
    if (train.periods_ && *train.periods_ + *test.periods_ <= n) {
      const bt::FoldLengths lengths{*train.periods_, *test.periods_, *test.periods_, 0};
      for (const auto& f : rolling ? bt::folds<bt::Rolling>(n, lengths) : bt::folds<bt::Expanding>(n, lengths)) {
        folds.push_back({f.index_, f.test_, {}, f.train_.begin_, f.train_.begin_});
      }
    } else if (!train.periods_) {
      folds = span_folds(p, anchor, limit, train, test, test, rolling, true, std::nullopt);
    }
    if (folds.empty()) {
      throw Refused(
        "no fold fits the history's " + std::to_string(n) + " periods", "give a shorter walk_forward.train"
      );
    }
    const auto count = folds.size();
    check_count(count * sets + count);
    // Each fold's training bars once; then each set's warm-up against them, the message made only to refuse.
    std::vector<std::size_t> have;
    for (const auto& f : folds) {
      have.push_back(instants(*prepared, p, f.train_warmup_, f.train_.begin_));
      // Whatever the sets: a training replay needs 2 bars to measure a return (an empty one has no bar to date).
      if (have.back() < 2) {
        throw Refused(
          "fold " + std::to_string(f.index_) + ": the training window holds " + std::to_string(have.back()) +
            " bars, fewer than 2",
          "give a longer walk_forward.train"
        );
      }
      if (f.train_.end_ - f.train_.begin_ < 2) {
        throw Refused(
          "fold " + std::to_string(f.index_) + ": the test holds fewer than 2 periods",
          "give a longer walk_forward.test"
        );
      }
    }
    for (std::size_t g = 0; g < sets; ++g) {
      const auto need = warmup_of<Strategy>(s, *prepared, grid_set(grid, g));
      const auto k = static_cast<std::size_t>(
        std::find_if(
          have.begin(), have.end(),
          [&need](std::size_t bars) {
            return need && bars < *need + 2;
          }
        ) -
        have.begin()
      );
      if (k < count) {
        throw Refused(
          "set " + std::to_string(g) + " " + params_json(grid_set(grid, g)) + ", fold " + std::to_string(k) +
            ": the training window holds " + std::to_string(have[k]) + " bars, and the program needs " +
            std::to_string(*need + 2),
          "give a longer walk_forward.train"
        );
      }
    }
    // Stage 1: every set on every training window, no history; the best of each fold by the objective.
    std::vector<Task> tasks;
    for (const auto& f : folds) {
      for (std::size_t g = 0; g < sets; ++g) {
        tasks.push_back(window_task(
          prepared, p, f.index_ * sets + g, grid_set(grid, g), f.index_, "train", f.train_warmup_, f.train_warmup_,
          f.train_.begin_
        ));
      }
    }
    tally.refused_.assign(sets, false);
    std::vector<std::optional<Best>> best(count);
    std::vector<std::size_t> eligible(count, 0);
    run_list<Strategy>(
      s, std::move(tasks), jobs, fills_of, measures,
      [](const auto&, const auto&) {
        return KeepNothing{};
      },
      BuildNothing{},
      [&](auto&& done) {
        const auto k = done.replay_.index_ / sets;
        tally.take(done, done.replay_.index_ % sets, false);
        eligible[k] += choose(best[k], *objective, direction == "min", done.replay_, done.figures_, done.halted_);
      },
      hooks
    );
    // Stage 2: the choice on the test window, its training window as history; the results kept for the chain.
    struct Tested {
      std::size_t index_{0};
      bool failed_{false}, halted_{false};
      std::vector<std::pair<std::string, double>> figures_;
      std::optional<bt::ReplayResult> result_;
      Counters counters_;
    };
    std::vector<std::optional<Tested>> tested(count);
    tasks.clear();
    for (const auto& f : folds) {
      if (best[f.index_]) {
        tasks.push_back(window_task(
          prepared, p, count * sets + tasks.size(), best[f.index_]->params_, f.index_, "test", f.train_warmup_,
          f.train_.begin_, f.train_.end_
        ));
      }
    }
    run_list<Strategy>(
      s, std::move(tasks), jobs, fills_of, measures,
      [](const bt::ReplayResult& result, const auto&) {
        return std::optional<bt::ReplayResult>(result);
      },
      BuildNothing{},
      [&](auto&& done) {
        tally.take(done, best[done.replay_.window_->fold_]->index_ % sets, false);
        tested[done.replay_.window_->fold_] = Tested{
          done.replay_.index_, done.failed_.has_value(), done.halted_, done.figures_, std::move(done.kept_),
          done.counters_
        };
      },
      hooks
    );
    // The chain: null when a fold has no test, a test failed or halted, has no curve point, or follows no capital.
    std::string reason;
    std::string chained = "null";
    std::string warnings = "[]";
    Counters sum;
    std::vector<const bt::ReplayResult*> results;
    for (std::size_t k = 0; k < count && reason.empty(); ++k) {
      const auto& t = tested[k];
      const auto* curve = t && t->result_ ? &t->result_->equity_ : nullptr;
      reason = !t ?
        "unchosen" :
        t->failed_ ?
        "failed" :
        t->halted_ ?
        "halted" :
        curve->empty() ?
        "empty" :
        curve->front().equity_.coefficient() <= 0 || (k + 1 < count && curve->back().equity_.coefficient() <= 0) ?
        "insolvent" :
        "";
      if (reason.empty()) {
        results.push_back(&*t->result_);
        add(sum, t->counters_);
      }
    }
    if (reason.empty()) {
      try {
        const auto chain = bt::chain_results(results);
        const auto fills = fills_of(prepared->fills_);
        const auto input = bt::measure_input(chain, fills, prepared->periods_);
        SessionFacts session{Strategy::atls_session, sum.opened_, sum.closed_, sum.outside_, prepared->open_point_};
        const auto values = run_measures(measures, input);
        chained =
          one_line(report_json(chain, input.report_, sum.pricing_, session, reads_data<Strategy>::value, {}, values));
      } catch (const std::bad_alloc&) {
        throw;
      } catch (const std::exception& e) {
        reason = "chain_unavailable";
        warnings = "[{\"code\":\"chain_unavailable\",\"message\":" + quoted(e.what()) + "}]";
      }
    }
    o << tally.head(method, jobs, "null", objective, direction)
      << ",\"scheme\":" << quoted(rolling ? "rolling" : "expanding") << ",\"train\":" << echo(train)
      << ",\"test\":" << echo(test) << ",\"train_replays\":"
      << count * static_cast<std::size_t>(std::count(tally.refused_.begin(), tally.refused_.end(), false))
      << ",\"uncovered_from\":" << uncovered(folds.back().train_.end_) << ",\"folds\":[";
    for (const auto& f : folds) {
      const auto& t = tested[f.index_];
      const auto& b = best[f.index_];
      const auto at = [&](std::size_t period, bool last) {
        return quoted(iso(events[last ? p.first_[period] - 1 : p.first_[period]].bar_.timestamp_));
      };
      o << (f.index_ == 0 ? "" : ",") << "{\"fold\":" << f.index_ << ",\"train_from\":" << at(f.train_warmup_, false)
        << ",\"train_to\":" << at(f.train_.begin_, true) << ",\"test_from\":" << at(f.train_.begin_, false)
        << ",\"test_to\":" << at(f.train_.end_, true) << ",\"train_periods\":" << f.train_.begin_ - f.train_warmup_
        << ",\"test_periods\":" << f.train_.end_ - f.train_.begin_ << ",\"eligible\":" << eligible[f.index_]
        << ",\"chosen\":" << (b ? std::to_string(b->index_ % sets) : "null") << ",\"params\":"
        << (b ? params_json(b->params_) : "null") << ",\"objective_value\":" << (b ? number(b->value_) : "null")
        << ",\"test_index\":" << (t ? std::to_string(t->index_) : "null")
        << ",\"test_total_return\":" << (t ? number(figure(t->figures_, "total_return")) : "null")
        << ",\"test_max_drawdown\":" << (t ? number(figure(t->figures_, "max_drawdown")) : "null")
        << ",\"test_halted\":" << (t ? (t->halted_ ? "true" : "false") : "null") << "}";
    }
    o << "],\"chained\":" << chained << ",\"chained_null_reason\":"
      << (reason.empty() ? std::string("null") : "\"" + reason + "\"") << ",\"warnings\":" << warnings;
  }
  if (method == "bootstrap") {
    const auto scheme = s.get_or("bootstrap.scheme", "circular");
    const auto kind = scheme == "moving" ?
      atl::analytics::BlockScheme::MOVING :
      scheme == "stationary" ?
      atl::analytics::BlockScheme::STATIONARY :
      atl::analytics::BlockScheme::CIRCULAR;
    const auto join = s.get_or("bootstrap.join", "returns");
    if (join != "returns" && join != "differences" && join != "levels") {
      throw std::invalid_argument("bootstrap.join is returns, differences or levels, not " + join);
    }
    const auto paths = static_cast<std::size_t>(whole_of(s.get_or("bootstrap.paths", "100"), "bootstrap.paths"));
    if (paths < 1 || paths > 10000) {
      throw std::invalid_argument("bootstrap.paths is 1 to 10000");
    }
    std::uint64_t seed = 0;
    if (const auto given = s.get("bootstrap.seed")) {
      const auto value = whole_of(*given, "bootstrap.seed");
      if (value < 0 || value > (1LL << 53) - 1) {
        throw std::invalid_argument("bootstrap.seed is 0 to 2^53 - 1");
      }
      seed = static_cast<std::uint64_t>(value);
    } else {
      std::random_device device;
      seed = ((static_cast<std::uint64_t>(device()) << 32) | device()) & ((std::uint64_t{1} << 53) - 1);
    }
    // A path rebuilds daily closes, and nothing else a bar or a program could read: refused here too, for a driver
    // called without the run spec's checks.
    const bool flat = std::all_of(events.begin(), events.end(), [](const atl::strategy::BarEvent& e) {
      const auto& b = e.bar_;
      return e.interval_ == atl::BarInterval::ONE_DAY && (!b.open_ || *b.open_ == b.close_) && b.high_ == b.close_ &&
        b.low_ == b.close_;
    });
    if (!flat || Strategy::atls_session || reads_data<Strategy>::value || s.get_or("orders.type", "") == "limit") {
      throw Refused(
        "a bootstrap path rebuilds daily closes only: no bar with its own open, high or low, no on_open, on_close or "
        "data {}, no limit orders",
        "give wide tables of closes, or a CSV with bars.layout = \"wide\""
      );
    }
    // Each symbol's bar on each period, found by its id (a long source orders a date's bars by time, then symbol):
    // every symbol one bar on every period.
    const auto symbols = prepared->symbols_.size();
    const auto& ids = prepared->ids_;
    std::vector<std::vector<std::size_t>> at(symbols, std::vector<std::size_t>(n));
    std::vector<std::vector<atl::Decimal64>> closes(symbols);
    for (std::size_t k = 0; k < n; ++k) {
      std::vector<std::size_t> seen(symbols, 0);
      for (auto i = p.first_[k]; i < p.first_[k + 1]; ++i) {
        const auto j = static_cast<std::size_t>(std::find(ids.begin(), ids.end(), events[i].instrument_) - ids.begin());
        if (j < symbols) {
          at[j][k] = i;
          ++seen[j];
        }
      }
      for (std::size_t j = 0; j < symbols; ++j) {
        if (seen[j] != 1) {
          throw Refused(
            prepared->symbols_[j] + (seen[j] == 0 ? " has no bar on " : " has two bars on ") +
              std::to_string(p.dates_[k]),
            "give every symbol a close on every date, or narrow replay.from and replay.to"
          );
        }
        closes[j].push_back(events[at[j][k]].bar_.close_);
      }
    }
    const bool fixed = kind != atl::analytics::BlockScheme::STATIONARY;
    std::size_t block = 0;
    if (const auto given = s.get("bootstrap.block")) {
      block = static_cast<std::size_t>(whole_of(*given, "bootstrap.block"));
      if (block < 1 || n < 3) {
        throw Refused("bootstrap needs a block of 1 or more and 3 periods or more", "give bootstrap.block = 1 or more");
      }
    } else {
      throw Refused("bootstrap draws blocks of bootstrap.block periods: give one", "give bootstrap.block = N");
    }
    if (fixed && block > n - 1) {
      throw Refused(
        "bootstrap.block " + std::to_string(block) + " is above the history's " + std::to_string(n - 1) + " returns",
        "give a smaller bootstrap.block"
      );
    }
    // The metrics the user names for intervals: report figures or measures, checked before any path.
    const auto level = std::stod(s.get_or("bootstrap.level", "0.95"));
    if (!(level > 0 && level < 1)) {
      throw std::invalid_argument("bootstrap.level is between 0 and 1, not " + s.get_or("bootstrap.level", ""));
    }
    const auto wanted = list_of(s.get_or("bootstrap.intervals", ""));
    const auto metrics = metric_names(measures);
    for (const auto& name : wanted) {
      const bool known = std::find(metrics.begin(), metrics.end(), name) != metrics.end();
      if (!known) {
        throw Refused(
          "bootstrap.intervals names no figure of the report and no measure: '" + name + "'",
          "name report figures (total_return, sharpe_annualized, ...) or measures.NAME"
        );
      }
    }
    std::string warnings = "[]";
    if (const auto lookback = warmup_of<Strategy>(s, *prepared, {}); lookback && block < *lookback) {
      warnings = "[{\"code\":\"block_below_lookback\",\"block\":" + std::to_string(block) +
        ",\"lookback\":" + std::to_string(*lookback) + "}]";
    }
    std::vector<Task> tasks;
    for (std::size_t k = 0; k <= paths; ++k) {
      tasks.push_back(
        Task{
          Replay{k, {}, std::nullopt, PathOf{k, seed, std::nullopt, Drawn{scheme, block, join}}}, prepared, nullptr, 0,
          std::numeric_limits<std::size_t>::max(), std::nullopt
        }
      );
    }
    // Path `p`: the returns drawn for every symbol at once, the closes rebuilt on each one's grid; path 0 the identity.
    const auto build = [&](Task& task) {
      const auto number = task.replay_.index_;
      std::vector<std::size_t> draw(n - 1);
      std::iota(draw.begin(), draw.end(), std::size_t{0});
      if (number > 0) {
        draw = atl::analytics::bootstrap_indices(n - 1, kind, block, seed, number);
      }
      auto path = events;
      for (std::size_t j = 0; j < symbols; ++j) {
        const auto rebuilt = bt::resample_closes(closes[j], prepared->config_.instruments_[j].price_rule_, draw);
        for (std::size_t k = 0; k < n; ++k) {
          auto& bar = path[at[j][k]].bar_;
          bar.open_ = bar.high_ = bar.low_ = bar.close_ = rebuilt[k];
        }
      }
      task.events_ = std::make_shared<const std::vector<atl::strategy::BarEvent>>(std::move(path));
    };
    tally.refused_.assign(1, false);
    std::string history = "null";
    run_list<Strategy>(
      s, std::move(tasks), jobs, fills_of, measures,
      [](const auto&, const auto&) {
        return KeepNothing{};
      },
      build,
      [&](auto&& done) {
        tally.take(done, 0, done.replay_.index_ > 0);
        if (done.replay_.index_ == 0 && !done.failed_) {
          history = by_figure(names, [&done](const std::string& name) {
            return number(figure(done.figures_, name));
          });
        }
      },
      hooks
    );
    o << tally.head(method, jobs, std::to_string(paths + 1), objective, direction) << ",\"scheme\":" << quoted(scheme)
      << ",\"block\":" << block << ",\"join\":" << quoted(join) << ",\"seed\":" << seed << ",\"history\":" << history
      << ",\"warnings\":" << warnings << ",\"halted\":" << tally.halted_ << ",\"intervals\":{";
    // Over paths 1 to N: a path without the figure (failed, halted, undefined) counts as null.
    for (std::size_t k = 0; k < wanted.size(); ++k) {
      std::vector<double> values;
      for (const auto& row : tally.spread_) {
        if (std::isfinite(figure(row, wanted[k]))) {
          values.push_back(figure(row, wanted[k]));
        }
      }
      std::sort(values.begin(), values.end());
      o << (k == 0 ? "" : ",") << quoted(wanted[k]) << ":{\"level\":" << number(level)
        << ",\"lower\":" << number(quantile(values, (1 - level) / 2)) << ",\"median\":" << number(quantile(values, 0.5))
        << ",\"upper\":" << number(quantile(values, (1 + level) / 2)) << ",\"defined\":" << values.size()
        << ",\"null\":" << paths - values.size() << "}";
    }
    o << "}";
  }
  if (method == "windows") {
    o << ",\"halted\":" << tally.halted_;
  }
  if (method == "windows" && !statistics.empty()) {
    o << ",\"summary\":" << by_figure(names, [&](const std::string& name) {
      std::vector<double> values;
      std::size_t nulls = 0;
      for (const auto& row : tally.spread_) {
        const auto x = figure(row, name);
        std::isfinite(x) ? values.push_back(x) : (void)++nulls;
      }
      return spread_json(values, nulls, statistics);
    });
  }
  std::cout << "{\"summary\":{" << o.str() << "}}\n";
  std::cout.flush();
  return std::cout ? 0 : 1;
}

} // namespace replay
} // namespace cmdatl
