#pragma once

#include "./replay.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <new>
#include <thread>

/**
 * @file
 * @brief Many replays in one driver (`evaluate.method=grid`): a description per replay, the data loaded once, one
 *   function that runs one replay (`replay_one`), run on the calling thread or on `evaluate.jobs` threads, and one JSON
 *   line per replay in index order, then a summary line.
 *
 * The index order: grid names in byte order, values in list order, the last name varying fastest; set `g` of `G`, path
 * `p`, replay `p * G + g` (`grid_set`).
 *
 * With `evaluate.jobs` above 1 the program's own C++ (its `extern` functions and indicators, a market-price rule) runs
 * on several threads at once and must be thread-safe; nothing here changes the locale.
 */

namespace cmdatl {
namespace replay {

/** @brief The most replays an evaluation known in advance runs. */
inline constexpr std::size_t MAX_REPLAYS = 100000;

/**
 * @brief Refuses an evaluation of more replays than `MAX_REPLAYS`, before the first one runs.
 * @param total The replays known in advance.
 */
inline void check_count(std::size_t total) {
  if (total > MAX_REPLAYS) {
    throw std::invalid_argument(
      "the evaluation makes " + std::to_string(total) + " replays, more than " + std::to_string(MAX_REPLAYS)
    );
  }
}

/**
 * @brief Whether a text is a JSON number.
 * @param text The text.
 * @return Whether it is.
 */
inline bool json_number(const std::string& text) {
  std::size_t at = text.size() > 0 && text[0] == '-' ? 1 : 0;
  const auto digits = [&text, &at]() {
    const auto from = at;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
      ++at;
    }
    return at - from;
  };
  const auto start = at;
  if (digits() == 0 || (text[start] == '0' && at - start > 1)) {
    return false;
  }
  if (at < text.size() && text[at] == '.' && (++at, digits() == 0)) {
    return false;
  }
  if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
    ++at;
    at += at < text.size() && (text[at] == '+' || text[at] == '-') ? 1 : 0;
    if (digits() == 0) {
      return false;
    }
  }
  return at == text.size();
}

/** @brief A grid: each parameter's name and values (as `param.NAME` text), names in byte order. */
using Grid = std::vector<std::pair<std::string, std::vector<std::string>>>;

/**
 * @brief The grid of `grid.NAME=V1,V2,...` settings: each value a JSON number, no two of a list the same `double`.
 * @param s The settings.
 * @return The grid.
 */
inline Grid grid_of(const Settings& s) {
  Grid grid;
  for (const auto& [name, list] : s.with_prefix("grid.")) {
    std::vector<std::string> values;
    std::vector<double> numbers;
    for (std::size_t from = 0; from <= list.size();) {
      const auto comma = std::min(list.find(',', from), list.size());
      values.push_back(list.substr(from, comma - from));
      try {
        numbers.push_back(json_number(values.back()) ? std::stod(values.back()) : std::nan(""));
      } catch (const std::exception&) {
        numbers.push_back(std::nan(""));
      }
      if (std::isnan(numbers.back())) {
        throw std::invalid_argument("grid." + name + ": '" + values.back() + "' is not a number");
      }
      from = comma + 1;
    }
    std::sort(numbers.begin(), numbers.end());
    if (std::adjacent_find(numbers.begin(), numbers.end()) != numbers.end()) {
      throw std::invalid_argument("grid." + name + " lists one value twice");
    }
    grid.emplace_back(name, std::move(values));
  }
  std::sort(grid.begin(), grid.end());
  return grid;
}

/**
 * @brief How many parameter sets a grid makes: the product of its lists' sizes, 1 without a grid.
 * @param grid The grid.
 * @return The count, refused past `MAX_REPLAYS`.
 */
inline std::size_t set_count(const Grid& grid) {
  std::size_t sets = 1;
  for (const auto& entry : grid) {
    if (entry.second.size() > MAX_REPLAYS / sets) {
      throw std::invalid_argument("the grid makes more than " + std::to_string(MAX_REPLAYS) + " parameter sets");
    }
    sets *= entry.second.size();
  }
  return sets;
}

/**
 * @brief The parameters of set `g`: names in byte order, values in list order, the last name varying fastest.
 * @param grid The grid.
 * @param g The set, from 0.
 * @return Each name with its value.
 */
inline std::vector<std::pair<std::string, std::string>> grid_set(const Grid& grid, std::size_t g) {
  std::vector<std::pair<std::string, std::string>> out(grid.size());
  for (std::size_t k = grid.size(); k-- > 0;) {
    const auto& values = grid[k].second;
    out[k] = {grid[k].first, values[g % values.size()]};
    g /= values.size();
  }
  return out;
}

/** @brief A replay to run: its description and the data it replays. */
struct Task {
  /** @brief The replay. */
  Replay replay_;
  /** @brief What its replays share. */
  std::shared_ptr<const Prepared> prepared_;
  /** @brief The events fed, when not the prepared ones (a bootstrap path's). */
  std::shared_ptr<const std::vector<atl::strategy::BarEvent>> events_;
  /** @brief The positions of the events fed, `[first_, last_)`, all of them by default. */
  std::size_t first_{0}, last_{std::numeric_limits<std::size_t>::max()};
  /** @brief Where trading starts, for a replay fed history first. */
  std::optional<std::chrono::system_clock::time_point> trade_from_;
};

/**
 * @brief A part of a vector, as a range.
 * @tparam T The element.
 */
template <typename T>
struct Part {
  /** @brief The first element and one past the last. */
  const T *begin_, *end_;
  /**
   * @brief The first.
   * @return It.
   */
  const T* begin() const {
    return begin_;
  }
  /**
   * @brief One past the last.
   * @return It.
   */
  const T* end() const {
    return end_;
  }
};

/** @brief A refusal of an evaluation before any replay (`evaluate.refused`), with the key to change. */
struct Refused : std::invalid_argument {
  /**
   * @brief Takes the message and what to do.
   * @param message The message.
   * @param next The key to change.
   */
  Refused(const std::string& message, std::string next) : std::invalid_argument(message), next_(std::move(next)) {}
  /** @brief The key to change. */
  std::string next_;
};

/** @brief The replays of a grid on one loaded data set, handed out by an atomic counter. */
class GridSource {
public:
  /**
   * @brief Takes the data and the grid.
   * @param prepared The data.
   * @param grid The grid.
   */
  GridSource(std::shared_ptr<const Prepared> prepared, Grid grid)
  : prepared_(std::move(prepared)), grid_(std::move(grid)), sets_(set_count(grid_)) {}

  /**
   * @brief The next replay, lock-free.
   * @param stop The runner's stop flag (unused: nothing here waits).
   * @return It, or none after the last.
   */
  std::optional<Task> next(const std::atomic<bool>& stop) {
    (void)stop;
    const auto index = next_.fetch_add(1);
    if (index >= sets_) {
      return std::nullopt;
    }
    return Task{
      Replay{index, grid_set(grid_, index), std::nullopt, std::nullopt}, prepared_, nullptr, 0,
      std::numeric_limits<std::size_t>::max(), std::nullopt
    };
  }

private:
  std::shared_ptr<const Prepared> prepared_;
  Grid grid_;
  std::size_t sets_;
  std::atomic<std::size_t> next_{0};
};

/**
 * @brief A finished replay, as its line and what the summary and the caller need of it.
 * @tparam Kept What its `keep` returned.
 */
template <typename Kept>
struct Finished {
  /** @brief The replay. */
  Replay replay_;
  /** @brief Its JSON line, without the newline. */
  std::string line_;
  /** @brief Set when it produced no report: how far it got. */
  std::optional<ReplayStage> failed_;
  /** @brief Whether it halted. */
  bool halted_{false};
  /** @brief The figures an objective can name. */
  std::vector<std::pair<std::string, double>> figures_;
  /** @brief What `keep` returned. */
  Kept kept_{};
  /** @brief The strategy's counts over the traded events. */
  Counters counters_;
};

/**
 * @brief Parameters as a JSON object, each value its text (a JSON number).
 * @param params The parameters.
 * @return The object.
 */
inline std::string params_json(const std::vector<std::pair<std::string, std::string>>& params) {
  std::string out = "{";
  for (const auto& [name, value] : params) {
    out += (out.size() == 1 ? "" : ",") + quoted(name) + ":" + value;
  }
  return out + "}";
}

/**
 * @brief `report_json`'s text on one line: every newline is structural (`quoted` escapes them in strings), dropped with
 *   the indent after it.
 * @param report The text.
 * @return The line.
 */
inline std::string one_line(const std::string& report) {
  std::string out;
  for (std::size_t at = 0; at < report.size(); ++at) {
    if (report[at] != '\n') {
      out.push_back(report[at]);
      continue;
    }
    while (at + 1 < report.size() && report[at + 1] == ' ') {
      ++at;
    }
  }
  return out;
}

/**
 * @brief A replay's JSON line: `index`, `params`, `window`, `path`, then `report` (`report_json`'s text on one line)
 *   or `"report":null` and `failure`.
 * @param replay The replay.
 * @param report The report, or null.
 * @param failure How far it got and its message, when it failed.
 * @return The line.
 */
inline std::string line_of(
  const Replay& replay, const std::string* report, const std::optional<std::pair<ReplayStage, std::string>>& failure
) {
  std::string out = "{\"index\":" + std::to_string(replay.index_) + ",\"params\":" + params_json(replay.params_);
  out += ",\"window\":";
  if (const auto& w = replay.window_) {
    out += "{\"fold\":" + std::to_string(w->fold_) + ",\"role\":" + quoted(w->role_) + ",\"warm_from\":" +
      (w->warm_from_ ? quoted(iso(*w->warm_from_)) : "null") + ",\"from\":" + quoted(iso(w->from_)) +
      ",\"to\":" + quoted(iso(w->to_)) + ",\"periods\":" + std::to_string(w->periods_) + "}";
  } else {
    out += "null";
  }
  out += ",\"path\":";
  if (const auto& p = replay.path_) {
    out += "{\"number\":" + std::to_string(p->number_) +
      ",\"seed\":" + (p->seed_ ? std::to_string(*p->seed_) : std::string("null")) +
      ",\"id\":" + (p->id_ ? quoted(*p->id_) : std::string("null")) +
      (p->drawn_ ? ",\"scheme\":" + quoted(p->drawn_->first) + ",\"block\":" + std::to_string(p->drawn_->second) : "") +
      "}";
  } else {
    out += "null";
  }
  out += ",\"report\":";
  if (report == nullptr) {
    return out + "null,\"failure\":{\"kind\":\"" + (failure->first == ReplayStage::PARAMS ? "params" : "replay") +
      "\",\"message\":" + quoted(failure->second) + "}}";
  }
  return out + one_line(*report) + "}";
}

/**
 * @brief Runs one task and makes its line: an ordinary `std::exception` is that replay's failure; `std::bad_alloc`
 *   and anything that is not a `std::exception` pass on, for the runner to stop the batch.
 * @tparam Strategy A strategy generated by `atlc`.
 * @param s The settings.
 * @param task The task; its data is released before the line is made.
 * @param fills_of Gives the fill model.
 * @param measures The report's measures.
 * @param keep Keeps what the caller needs of each result.
 * @return The finished replay.
 */
template <typename Strategy, typename Fills, typename Measures, typename Keep>
auto finish_one(const Settings& s, Task task, Fills& fills_of, const Measures& measures, Keep& keep) {
  using Kept = decltype(keep(
    std::declval<const atl::backtest::ReplayResult&>(), std::declval<const atl::backtest::NextBarFillModel&>()
  ));
  Finished<Kept> done;
  done.replay_ = std::move(task.replay_);
  ReplayStage stage = ReplayStage::REPLAY;
  std::optional<Outcome<Kept>> outcome;
  std::string message;
  try {
    const auto& prepared = *task.prepared_;
    const auto& all = task.events_ ? *task.events_ : prepared.events_;
    const Part<atl::strategy::BarEvent> part{
      all.data() + std::min(task.first_, all.size()), all.data() + std::min(task.last_, all.size())
    };
    auto config = prepared.config_;
    config.trade_from_ = task.trade_from_;
    outcome.emplace(replay_one<Strategy>(s, prepared, part, config, done.replay_, fills_of, measures, keep, stage));
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& e) {
    message = e.what();
  }
  task.prepared_.reset();
  if (!outcome) {
    done.failed_ = stage;
    done.line_ = line_of(done.replay_, nullptr, std::make_pair(stage, message));
    return done;
  }
  done.line_ = line_of(done.replay_, &outcome->report_, std::nullopt);
  done.halted_ = outcome->halted_;
  done.figures_ = std::move(outcome->figures_);
  done.kept_ = std::move(outcome->kept_);
  done.counters_ = outcome->counters_;
  return done;
}

/** @brief The runner's test hooks, each called when set. */
struct RunnerHooks {
  /** @brief Before each thread is started, with its number from 1. */
  std::function<void(std::size_t)> starting_;
};

/**
 * @brief Runs every task of a source, `jobs` at once, and hands each finished replay to `sink` in index order.
 *
 * With `jobs` 1 everything runs on the calling thread. Above, `jobs - 1` threads and the calling thread take tasks; a
 * task of index `i` waits until `i - next_to_print < 4 * jobs`, so at most `4 * jobs` finished replays wait in a ring
 * for their turn. The sink runs under the runner's lock, one replay at a time. Anything that escapes `work`, the
 * source or the sink stops the batch: no new task is taken, waiting tasks are dropped, running replays finish; every
 * thread is joined by the calling thread before this returns or throws, and the first such exception is rethrown.
 *
 * @tparam Source Has `std::optional<Task> next(const std::atomic<bool>& stop)`.
 * @param source The tasks, with increasing indices from 0.
 * @param jobs How many run at once, at least 1.
 * @param work Runs a task: `Finished<Kept> work(Task)`.
 * @param sink Takes each finished replay, in index order.
 * @param hooks The test hooks.
 */
template <typename Source, typename Work, typename Sink>
void run_replays(Source& source, std::size_t jobs, Work& work, Sink& sink, const RunnerHooks& hooks = {}) {
  using Done = decltype(work(std::declval<Task>()));
  const std::size_t window = 4 * jobs;
  std::mutex mutex;
  std::condition_variable wake;
  std::atomic<bool> stop{false};
  std::size_t next_to_print = 0;
  std::vector<std::optional<Done>> ring(window);
  std::exception_ptr fatal;
  const auto fail = [&](std::exception_ptr error) {
    stop = true;
    {
      const std::lock_guard<std::mutex> lock(mutex);
      if (!fatal) {
        fatal = std::move(error);
      }
    }
    wake.notify_all();
  };
  const auto worker = [&]() {
    try {
      while (!stop) {
        auto task = source.next(stop);
        if (!task) {
          return;
        }
        const auto index = task->replay_.index_;
        {
          std::unique_lock<std::mutex> lock(mutex);
          wake.wait(lock, [&]() {
            return stop || index - next_to_print < window;
          });
          if (stop) {
            return;
          }
        }
        auto done = work(std::move(*task));
        task.reset();
        {
          const std::lock_guard<std::mutex> lock(mutex);
          if (stop) {
            return;
          }
          ring[index % window] = std::move(done);
          // Each value leaves its slot before the sink sees it; a sink that throws stops the batch under the lock.
          while (ring[next_to_print % window]) {
            auto current = std::move(*ring[next_to_print % window]);
            ring[next_to_print % window].reset();
            ++next_to_print;
            try {
              sink(std::move(current));
            } catch (...) {
              stop = true;
              throw;
            }
          }
        }
        wake.notify_all();
      }
    } catch (...) {
      fail(std::current_exception());
    }
  };
  std::vector<std::thread> threads;
  // Joins every thread on every path; on any but the normal end it first stops the batch and wakes the waiters.
  struct Joiner {
    std::vector<std::thread>& threads_;
    std::atomic<bool>& stop_;
    std::mutex& mutex_;
    std::condition_variable& wake_;
    bool normal_{false};
    ~Joiner() {
      if (!normal_) {
        stop_ = true;
        {
          const std::lock_guard<std::mutex> lock(mutex_);
        }
        wake_.notify_all();
      }
      for (auto& thread : threads_) {
        thread.join();
      }
    }
  };
  Joiner joiner{threads, stop, mutex, wake};
  threads.reserve(jobs - 1);
  for (std::size_t k = 1; k < jobs; ++k) {
    if (hooks.starting_) {
      hooks.starting_(k);
    }
    threads.emplace_back(worker);
  }
  worker();
  joiner.normal_ = true;
  for (auto& thread : threads) {
    thread.join();
  }
  threads.clear();
  if (fatal) {
    std::rethrow_exception(fatal);
  }
}

template <typename Strategy>
class StdinSource;

/** @brief The best replay of an evaluation by its objective. */
struct Best {
  /** @brief Its index. */
  std::size_t index_{0};
  /** @brief Its parameters. */
  std::vector<std::pair<std::string, std::string>> params_;
  /** @brief Its objective's value. */
  double value_{0};
};

/**
 * @brief Considers a replay for the best: one with a report, not halted, with a finite value of the objective, better
 *   than the best so far in the direction asked, a tie going to the lower index.
 * @param best The best so far, updated.
 * @param objective The figure compared.
 * @param minimize Whether lower is better.
 * @param replay The replay.
 * @param figures Its figures; none for a replay without a report.
 * @param halted Whether it halted.
 * @return Whether it was eligible: a report, not halted, a finite value.
 */
inline bool choose(
  std::optional<Best>& best, const std::string& objective, bool minimize, const Replay& replay,
  const std::vector<std::pair<std::string, double>>& figures, bool halted
) {
  const auto found = std::find_if(figures.begin(), figures.end(), [&objective](const auto& figure) {
    return figure.first == objective;
  });
  if (halted || found == figures.end() || !std::isfinite(found->second)) {
    return false;
  }
  const double value = found->second;
  const bool better = !best || (minimize ? value < best->value_ : value > best->value_) ||
    (value == best->value_ && replay.index_ < best->index_);
  if (better) {
    best = Best{replay.index_, replay.params_, value};
  }
  return true;
}

template <typename Strategy, typename Fills, typename Measures>
int loops_main(
  const Settings& s, const std::string& method, Fills& fills_of, const Measures& measures, std::size_t jobs,
  const std::optional<std::string>& objective, const std::string& direction, const RunnerHooks& hooks
);

/**
 * @brief The metrics an objective or an interval may name: the report's figures, then `measures.NAME` for each measure.
 * @tparam Measures A table of pairs whose first member is the measure's name.
 * @param measures The report's measures, the program's first, as the driver's table lists them.
 * @return The names.
 */
template <typename Measures>
std::vector<std::string> metric_names(const Measures& measures) {
  std::vector<std::string> out{
    "total_return", "sharpe_per_period", "sharpe_annualized", "sortino_per_period", "max_drawdown", "turnover",
    "executions"
  };
  for (const auto& measure : measures) {
    out.push_back(std::string("measures.") + measure.first);
  }
  return out;
}

/**
 * @brief Evaluates a compiled strategy on many replays (`evaluate.method=grid`, or a method of `loops.hpp`): the replays of `grid.NAME` settings
 *   on the data loaded once, run `evaluate.jobs` at once, one JSON line each in index order, then
 *   `{"summary":{...}}`; a failure that ends the batch prints `{"stopped":{"code":...,"message":...}}` and an `error:`
 *   line on standard error instead of the summary.
 * @tparam Strategy A strategy generated by `atlc`.
 * @tparam Fills A callable taking the settings' fill model and returning the one the replay uses.
 * @param s The settings.
 * @param fills_of Gives the fill model.
 * @param measures The report's measures; an objective may name one as `measures.NAME`.
 * @param input The descriptor `bars.source=stdin` reads.
 * @param hooks The runner's test hooks, or null.
 * @return 0 after the summary, 1 after a stop line.
 */
template <typename Strategy, typename Fills, typename Measures>
int evaluate_main(const Settings& s, Fills& fills_of, const Measures& measures, int input, const RunnerHooks* hooks) {
  const auto stopped = [](const std::string& message, const std::string& next = {}) {
    std::cout
      << "{\"stopped\":{\"code\":\"" << (next.empty() ? "replay.failed" : "evaluate.refused")
      << "\",\"message\":" << quoted(message) << (next.empty() ? "" : ",\"next\":" + quoted(next)) << "}}\n";
    std::cout.flush();
    std::cerr << "error: " << message << '\n';
    return 1;
  };
  try {
    const auto method = s.get_or("evaluate.method", "");
    if (method != "grid" && method != "windows" && method != "walk-forward" && method != "bootstrap") {
      throw std::invalid_argument("evaluate.method is grid, windows, walk-forward or bootstrap, not '" + method + "'");
    }
    const auto jobs = whole_of(s.get_or("evaluate.jobs", "1"), "evaluate.jobs");
    if (jobs < 1 || jobs > 256) {
      throw std::invalid_argument("evaluate.jobs is 1 to 256, not " + std::to_string(jobs));
    }
    const auto objective = s.get("evaluate.objective");
    const auto direction = s.get_or("evaluate.direction", "max");
    const auto figures = metric_names(measures);
    if (objective && std::find(figures.begin(), figures.end(), *objective) == figures.end()) {
      throw std::invalid_argument("evaluate.objective names no figure of the report: '" + *objective + "'");
    }
    if (direction != "max" && direction != "min") {
      throw std::invalid_argument("evaluate.direction is max or min, not '" + direction + "'");
    }
    if (method != "grid") {
      return loops_main<Strategy>(
        s, method, fills_of, measures, static_cast<std::size_t>(jobs), objective, direction,
        hooks == nullptr ? RunnerHooks{} : *hooks
      );
    }
    auto grid = grid_of(s);
    const auto sets = set_count(grid);
    check_count(sets);
    std::size_t replays = 0;
    std::size_t failed = 0;
    std::vector<bool> refused(sets, false);
    std::optional<Best> best;
    std::optional<std::size_t> paths;
    if (s.get_or("bars.source", "") == "stdin") {
      paths = 0;
    }
    auto keep = [](const atl::backtest::ReplayResult&, const atl::backtest::NextBarFillModel&) {
      return KeepNothing{};
    };
    auto work = [&](Task task) {
      return finish_one<Strategy>(s, std::move(task), fills_of, measures, keep);
    };
    auto sink = [&](Finished<KeepNothing>&& done) {
      std::cout << done.line_ << '\n';
      std::cout.flush();
      ++replays;
      if (done.failed_ == ReplayStage::PARAMS) {
        refused[done.replay_.index_ % sets] = true;
      } else if (done.failed_) {
        ++failed;
      }
      // The best over paths is an aggregate this version does not make.
      if (objective && !paths) {
        choose(best, *objective, direction == "min", done.replay_, done.figures_, done.halted_);
      }
    };
    const auto run = [&](auto& source) {
      run_replays(source, static_cast<std::size_t>(jobs), work, sink, hooks == nullptr ? RunnerHooks{} : *hooks);
    };
    if (paths) {
      StdinSource<Strategy> source(s, std::move(grid), input);
      run(source);
      paths = source.paths();
    } else {
      GridSource source(std::make_shared<const Prepared>(prepare<Strategy>(s)), std::move(grid));
      run(source);
    }
    const auto invalid = static_cast<std::size_t>(std::count(refused.begin(), refused.end(), true));
    std::ostringstream o;
    o << "{\"summary\":{\"method\":\"grid\",\"jobs\":" << jobs << ",\"param_sets\":" << sets
      << ",\"invalid\":" << invalid << ",\"tried\":" << sets - invalid << ",\"replays\":" << replays
      << ",\"paths\":" << (paths ? std::to_string(*paths) : "null") << ",\"failed\":" << failed
      << ",\"objective\":" << (objective ? quoted(*objective) : "null")
      << ",\"direction\":" << (objective ? quoted(direction) : "null") << ",\"best\":";
    if (best) {
      o << "{\"index\":" << best->index_ << ",\"params\":" << params_json(best->params_)
        << ",\"value\":" << number(best->value_) << "}";
    } else {
      o << "null";
    }
    std::cout << o.str() << "}}\n";
    std::cout.flush();
    return std::cout ? 0 : 1;
  } catch (const Refused& e) {
    return stopped(e.what(), e.next_);
  } catch (const std::exception& e) {
    return stopped(e.what());
  } catch (...) {
    return stopped("a replay threw something that is not a std::exception");
  }
}

} // namespace replay
} // namespace cmdatl

#include "./stdin.hpp"

// The methods `evaluate_main` hands every `evaluate.method` but `grid` to.
#include "./loops.hpp"
