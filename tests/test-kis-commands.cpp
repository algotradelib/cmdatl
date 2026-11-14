// The order commands on a KIS account, on the wire: every command runs as a `kis-command` child (the whole cmdatl
// command line on KIS's shipped setup, `tests/kis-command.cpp`) against loopback HTTPS servers standing in for KIS.
// A demo account has two servers, one behind the demo login's client and one behind the real login's, so a request
// on the wrong session fails by its server; a real account has one. Every login here is made up.
//
// Time: the child's live clock starts at 10:00 KST on the day before this machine's date in Seoul, a day the scripted
// calendar marks open, and moves with the steady clock. That day, not today, so the live clock always runs behind
// the system clock the KIS client stamps its replies with (between 14 and 38 hours), whatever the hour the test runs.
// Ahead of it, every holdings read would be stale to the runner, which measures freshness at the later of cmdatl's
// instant and a quote's stamp; behind it, the runner's own timeouts see restored orders as hours old, which changes
// none of the outcomes asserted here (each is decided by the listing's sweep or by once()'s checks on cmdatl's clock).
// The demo list's date and the calendar's anchor are the client's own, from the system clock: the calendar covers
// both days.

#include "./local_https_server.hpp"
#include "./support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

using support::Recorded;
using support::Scripted;

namespace {

namespace json = boost::json;

/** @brief The token request. */
constexpr const char* TOKEN = "/oauth2/tokenP";
/** @brief The holiday inquiry (the calendar). */
constexpr const char* HOLIDAY = "/uapi/domestic-stock/v1/quotations/chk-holiday";
/** @brief The current price: the quote and the price band. */
constexpr const char* PRICE = "/uapi/domestic-stock/v1/quotations/inquire-price";
/** @brief An instrument's reference data. */
constexpr const char* REFERENCE = "/uapi/domestic-stock/v1/quotations/search-stock-info";
/** @brief Holdings. */
constexpr const char* BALANCE = "/uapi/domestic-stock/v1/trading/inquire-balance";
/** @brief Buying power. */
constexpr const char* POWER = "/uapi/domestic-stock/v1/trading/inquire-psbl-order";
/** @brief A real account's working orders. */
constexpr const char* AMENDABLE = "/uapi/domestic-stock/v1/trading/inquire-psbl-rvsecncl";
/** @brief The daily inquiry: the dated history, and a demo account's working orders. */
constexpr const char* DAILY = "/uapi/domestic-stock/v1/trading/inquire-daily-ccld";
/** @brief A cash order. */
constexpr const char* ORDER = "/uapi/domestic-stock/v1/trading/order-cash";
/** @brief An amendment or cancellation. */
constexpr const char* AMEND = "/uapi/domestic-stock/v1/trading/order-rvsecncl";

/** @brief The demo account's login: account 12345678-01. */
const std::string DEMO_LOGIN =
  R"({"app_key":"made-up-demo-key","app_secret":"MADE-UP-DEMO-SECRET","account_number":"12345678","demo":true})";
/** @brief The real login: the market-data login of a demo account, and the account of a real one (87654321-01). */
const std::string REAL_LOGIN =
  R"({"app_key":"made-up-real-key","app_secret":"MADE-UP-REAL-SECRET","account_number":"87654321","demo":false})";

/** @brief The paths a demo trading server answers; the market server answers the others. */
const std::vector<std::string> ACCOUNT_PATHS = {BALANCE, POWER, AMENDABLE, DAILY, ORDER, AMEND};
/** @brief The market-data paths. */
const std::vector<std::string> MARKET_PATHS = {HOLIDAY, PRICE, REFERENCE};

/**
 * @brief A date as KIS writes it.
 * @param at An instant.
 * @return Its date in Seoul, `YYYYMMDD`.
 */
std::string compact_kst_date(cmdatl::Clock::time_point at) {
  const auto iso = cmdatl::kst_iso(at);
  return iso.substr(0, 4) + iso.substr(5, 2) + iso.substr(8, 2);
}

/**
 * @brief A time as KIS writes it.
 * @param at An instant.
 * @return Its time in Seoul, `HHMMSS`.
 */
std::string compact_kst_time(cmdatl::Clock::time_point at) {
  const auto iso = cmdatl::kst_iso(at);
  return iso.substr(11, 2) + iso.substr(14, 2) + (iso.size() >= 19 ? iso.substr(17, 2) : std::string("00"));
}

/**
 * @brief The live clock's start: 10:00 KST on the day before this machine's date in Seoul.
 * @return The instant.
 */
cmdatl::Clock::time_point trading_morning() {
  using Days = std::chrono::duration<std::int64_t, std::ratio<86400>>;
  const auto seoul = cmdatl::Clock::now() + std::chrono::hours(9);
  const auto day = std::chrono::floor<Days>(seoul.time_since_epoch()) - Days(1);
  return cmdatl::Clock::time_point(std::chrono::duration_cast<cmdatl::Clock::duration>(day)) + std::chrono::hours(1);
}

/** @brief An order the stand-in broker knows. */
struct BrokerOrder {
  /** @brief The order number. */
  std::string odno_;
  /** @brief The branch. */
  std::string branch_{"06010"};
  /** @brief Whether a buy. */
  bool buy_{true};
  /** @brief The symbol. */
  std::string symbol_{"005930"};
  /** @brief The order date, `YYYYMMDD`. */
  std::string date_;
  /** @brief The order time, `HHMMSS`. */
  std::string time_;
  /** @brief The quantity ordered. */
  long quantity_{1};
  /** @brief The limit. */
  long price_{70000};
  /** @brief The quantity filled. */
  long filled_{0};
  /** @brief Whether it ended unfilled in part or whole (cancelled). */
  bool ended_{false};
  /** @brief Whether the working list leaves it out though it rests (the demo list's lag). */
  bool unlisted_{false};

  /**
   * @brief What still rests.
   * @return The quantity.
   */
  long remaining() const {
    return ended_ ? 0 : quantity_ - filled_;
  }
};

/** @brief The end of a daily-inquiry reply, after its rows: the totals KIS sends beside them. */
const std::string DAILY_TAIL =
  R"(],"output2":{"tot_ord_qty":"0","tot_ccld_qty":"0","tot_ccld_amt":"0","prsm_tlex_smtl":"0","pchs_avg_pric":"0"}})";

/** @brief A KIS reply's common part. */
const std::string OK_HEAD = R"("rt_cd":"0","msg_cd":"MCA00000","msg1":"정상처리 되었습니다.")";

/**
 * @brief The stand-in broker behind the servers: what is held, the orders it knows, and how it answers each path.
 *
 * The test changes it between commands; the servers' threads read it under its lock.
 */
class Broker {
public:
  /** @brief The lock. */
  mutable std::mutex mutex_;
  /** @brief Shares of 005930 held. */
  long held_{0};
  /** @brief The orders, in the order placed. */
  std::vector<BrokerOrder> orders_;
  /** @brief The next order number. */
  long next_{10001};
  /** @brief The order time stamped on the next accepted order (the test's live clock). */
  cmdatl::Clock::time_point now_{};

  /**
   * @brief The holdings reply.
   * @return The body.
   */
  std::string balance() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string rows;
    if (held_ > 0) {
      const auto value = std::to_string(held_ * 70000);
      rows = R"({"pdno":"005930","prdt_name":"Samsung","hldg_qty":")" + std::to_string(held_) +
        R"(","ord_psbl_qty":")" + std::to_string(held_) + R"(","pchs_avg_pric":"70000.0000","pchs_amt":")" + value +
        R"(","prpr":"70000","evlu_amt":")" + value + R"(","evlu_pfls_amt":"0","evlu_pfls_rt":"0.00"})";
    }
    const auto total = std::to_string(held_ * 70000);
    return "{" + OK_HEAD + R"(,"ctx_area_fk100":"","ctx_area_nk100":"","output1":[)" + rows +
      R"(],"output2":[{"pchs_amt_smtl_amt":")" + total + R"(","evlu_amt_smtl_amt":")" + total +
      R"(","evlu_pfls_smtl_amt":"0"}]})";
  }

  /**
   * @brief One daily-inquiry row, with the exchange field the workbook spells `excg_id_dvsn_Cd` (required, KRX on
   *   demo).
   * @param order The order.
   * @return The row.
   */
  static std::string daily_row(const BrokerOrder& order) {
    return R"({"ord_dt":")" + order.date_ + R"(","ord_gno_brno":")" + order.branch_ + R"(","odno":")" + order.odno_ +
      R"(","orgn_odno":"0000000000","pdno":")" + order.symbol_ + R"(","prdt_name":"Samsung","sll_buy_dvsn_cd":")" +
      (order.buy_ ? "02" : "01") + R"(","ord_dvsn_cd":"00","ord_qty":")" + std::to_string(order.quantity_) +
      R"(","ord_unpr":")" + std::to_string(order.price_) + R"(","ord_tmd":")" + order.time_ + R"(","tot_ccld_qty":")" +
      std::to_string(order.filled_) + R"(","tot_ccld_amt":")" + std::to_string(order.filled_ * order.price_) +
      R"(","avg_prvs":")" + std::to_string(order.filled_ > 0 ? order.price_ : 0) + R"(","cncl_yn":"N","rmn_qty":")" +
      std::to_string(order.remaining()) + R"(","rjct_qty":"0","excg_dvsn_cd":"02","excg_id_dvsn_Cd":"KRX"})";
  }

  /**
   * @brief The daily inquiry's reply: on demo with `CCLD_DVSN=02` the working list (rows still resting), else every
   *   order dated inside the query's range.
   * @param request The request.
   * @return The body.
   */
  std::string daily(const Recorded& request) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto from = request.param("INQR_STRT_DT").value_or("");
    const auto to = request.param("INQR_END_DT").value_or("");
    const bool working = request.param("CCLD_DVSN").value_or("") == "02";
    std::string rows;
    for (const auto& order : orders_) {
      if (working ? order.remaining() <= 0 || order.unlisted_ : (order.date_ < from || to < order.date_)) {
        continue;
      }
      rows += (rows.empty() ? "" : ",") + daily_row(order);
    }
    return "{" + OK_HEAD + R"(,"ctx_area_fk100":"","ctx_area_nk100":"","output1":[)" + rows + DAILY_TAIL;
  }

  /**
   * @brief The real account's amendable-order reply: every order still resting.
   * @return The body.
   */
  std::string amendable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string rows;
    for (const auto& order : orders_) {
      if (order.remaining() <= 0 || order.unlisted_) {
        continue;
      }
      rows += (rows.empty() ? "" : ",") + std::string(R"({"ord_gno_brno":")") + order.branch_ + R"(","odno":")" +
        order.odno_ + R"(","orgn_odno":"","ord_dvsn_name":"limit","pdno":")" + order.symbol_ +
        R"(","prdt_name":"Samsung","rvse_cncl_dvsn_name":"","ord_qty":")" + std::to_string(order.quantity_) +
        R"(","ord_unpr":")" + std::to_string(order.price_) + R"(","ord_tmd":")" + order.time_ +
        R"(","tot_ccld_qty":")" + std::to_string(order.filled_) + R"(","tot_ccld_amt":")" +
        std::to_string(order.filled_ * order.price_) + R"(","psbl_qty":")" + std::to_string(order.remaining()) +
        R"(","sll_buy_dvsn_cd":")" + (order.buy_ ? "02" : "01") + R"(","ord_dvsn_cd":"00","excg_id_dvsn_cd":"KRX"})";
    }
    return "{" + OK_HEAD + R"(,"ctx_area_fk100":"","ctx_area_nk100":"","output":[)" + rows + "]}";
  }

  /**
   * @brief Accepts a cash order: a new resting order, numbered and stamped with the live clock.
   * @param request The request.
   * @return The reply.
   */
  Scripted accept(const Recorded& request) {
    const auto body = json::parse(request.body_).as_object();
    std::lock_guard<std::mutex> lock(mutex_);
    BrokerOrder order;
    order.odno_ = "00000" + std::to_string(next_++);
    order.buy_ = request.header("tr_id").substr(6) == "12U";
    order.symbol_ = std::string(body.at("PDNO").as_string());
    order.quantity_ = std::stol(std::string(body.at("ORD_QTY").as_string()));
    order.price_ = std::stol(std::string(body.at("ORD_UNPR").as_string()));
    order.date_ = compact_kst_date(now_);
    order.time_ = compact_kst_time(now_);
    orders_.push_back(order);
    return Scripted{
      200,
      R"({"rt_cd":"0","msg_cd":"APBK0013","msg1":"주문 전송 완료 되었습니다.","output":{"KRX_FWDG_ORD_ORGNO":")" +
        order.branch_ + R"(","ODNO":")" + order.odno_ + R"(","ORD_TMD":")" + order.time_ + R"("}})",
      {}
    };
  }

  /**
   * @brief Accepts a cancellation: the named order ends with what it filled.
   * @param request The request.
   * @return The reply.
   */
  Scripted cancel(const Recorded& request) {
    const auto body = json::parse(request.body_).as_object();
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string named(body.at("ORGN_ODNO").as_string());
    for (auto& order : orders_) {
      if (order.odno_ == named) {
        order.ended_ = true;
      }
    }
    const auto odno = "00000" + std::to_string(next_++);
    return Scripted{
      200,
      R"({"rt_cd":"0","msg_cd":"APBK0013","msg1":"주문 전송 완료 되었습니다.","output":{"KRX_FWDG_ORD_ORGNO":"06010",)"
      R"("ODNO":")" +
        odno + R"(","ORD_TMD":")" + compact_kst_time(now_) + R"("}})",
      {}
    };
  }

  /**
   * @brief Changes an order the broker knows.
   * @param odno Its number.
   * @param change What to do to it.
   */
  void change(const std::string& odno, const std::function<void(BrokerOrder&)>& change) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& order : orders_) {
      if (order.odno_ == odno) {
        change(order);
      }
    }
  }

  /**
   * @brief Adds an order placed elsewhere, resting.
   * @param order The order.
   */
  void add(BrokerOrder order) {
    std::lock_guard<std::mutex> lock(mutex_);
    orders_.push_back(std::move(order));
  }

  /**
   * @brief Fills part or all of an order: holdings move with it.
   * @param odno The order number.
   * @param quantity The quantity.
   */
  void fill(const std::string& odno, long quantity) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& order : orders_) {
      if (order.odno_ == odno) {
        order.filled_ += quantity;
        held_ += order.buy_ ? quantity : -quantity;
      }
    }
  }
};

/**
 * @brief The holiday reply: every day from three days before `day` to ten after is open.
 * @param day The live clock's day.
 * @return The body.
 */
std::string holidays(cmdatl::Clock::time_point day) {
  std::string rows;
  for (int offset = -3; offset <= 10; ++offset) {
    const auto date = compact_kst_date(day + std::chrono::hours(24 * offset));
    rows += (rows.empty() ? "" : ",") + std::string(R"({"bass_dt":")") + date +
      R"(","wday_dvsn_cd":"01","bzdy_yn":"Y","tr_day_yn":"Y","opnd_yn":"Y","sttl_day_yn":"Y"})";
  }
  return "{" + OK_HEAD + R"(,"ctx_area_nk":"","ctx_area_fk":"","output":[)" + rows + "]}";
}

/** @brief A cmdatl child: started with pipes on its stdout and stderr, killed and reaped if a test leaves it. */
class Child {
public:
  /**
   * @brief Starts it, with no descriptor but the three standard ones and a given environment.
   * @param argv The program and its arguments.
   * @param env The environment, `NAME=VALUE`.
   */
  Child(const std::vector<std::string>& argv, const std::vector<std::string>& env) {
    std::vector<char*> args;
    for (const auto& arg : argv) {
      args.push_back(const_cast<char*>(arg.c_str()));
    }
    args.push_back(nullptr);
    std::vector<char*> envp;
    for (const auto& entry : env) {
      envp.push_back(const_cast<char*>(entry.c_str()));
    }
    envp.push_back(nullptr);
    int out[2];
    int err[2];
    REQUIRE(::pipe(out) == 0);
    REQUIRE(::pipe(err) == 0);
    for (const int fd : {out[0], out[1], err[0], err[1]}) {
      ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    REQUIRE(posix_spawn_file_actions_init(&actions) == 0);
    REQUIRE(posix_spawnattr_init(&attr) == 0);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err[1], STDERR_FILENO);
    short flags = POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
#if defined(__APPLE__)
    // Only what the file actions name is inherited: none of the servers' sockets reaches the child (on glibc
    // before 2.34, which has no way to say so, the sockets asio opens without close-on-exec are inherited).
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#endif
    sigset_t defaults;
    sigemptyset(&defaults);
    for (const int sig : {SIGPIPE, SIGINT, SIGTERM, SIGHUP}) {
      sigaddset(&defaults, sig);
    }
    sigset_t unblocked;
    sigemptyset(&unblocked);
    posix_spawnattr_setflags(&attr, flags);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setsigmask(&attr, &unblocked);
    const int spawned = posix_spawn(&pid_, args[0], &actions, &attr, args.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    ::close(out[1]);
    ::close(err[1]);
    out_.reset(out[0]);
    err_.reset(err[0]);
    if (spawned != 0) {
      pid_ = -1;
    }
    REQUIRE(spawned == 0);
  }

  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;

  /** @brief Kills and reaps it if it still runs. */
  ~Child() {
    kill();
  }

  /** @brief Kills it with SIGKILL and reaps it, if it was not reaped yet. */
  void kill() {
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
      ::waitpid(pid_, &status_, 0);
      pid_ = -1;
      drain();
    }
  }

  /**
   * @brief Waits for it to end, reading both pipes meanwhile.
   * @param timeout The longest wait; past it the test fails, and the child is killed when the object goes.
   * @return Its wait status.
   */
  int wait(std::chrono::seconds timeout = std::chrono::seconds(30)) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    while (pid_ > 0) {
      const auto waited = ::waitpid(pid_, &status_, WNOHANG);
      if (waited == pid_) {
        pid_ = -1;
        break;
      }
      if (std::chrono::steady_clock::now() > until) {
        FAIL("kis-command did not end in " << timeout.count() << " s");
      }
      read_some(5);
    }
    drain();
    return status_;
  }

  /**
   * @brief Its process id.
   * @return It, or -1 once reaped.
   */
  pid_t pid() const noexcept {
    return pid_;
  }

  /** @brief What it wrote on stdout. */
  std::string out_text_;
  /** @brief What it wrote on stderr. */
  std::string err_text_;

private:
  /** @brief Reads whatever is left once it ended. */
  void drain() {
    while (out_.get() >= 0 || err_.get() >= 0) {
      if (!read_some(100)) {
        break;
      }
    }
  }

  /**
   * @brief Reads what either pipe has.
   * @param ms How long to wait for something.
   * @return Whether either pipe is still open.
   */
  bool read_some(int ms) {
    pollfd fds[2];
    nfds_t count = 0;
    for (const int fd : {out_.get(), err_.get()}) {
      if (fd >= 0) {
        fds[count++] = pollfd{fd, POLLIN, 0};
      }
    }
    if (count == 0) {
      return false;
    }
    if (::poll(fds, count, ms) <= 0) {
      return true;
    }
    for (nfds_t i = 0; i < count; ++i) {
      if (fds[i].revents == 0) {
        continue;
      }
      char chunk[4096];
      const auto n = ::read(fds[i].fd, chunk, sizeof(chunk));
      const bool is_out = fds[i].fd == out_.get();
      if (n <= 0) {
        (is_out ? out_ : err_).reset();
        continue;
      }
      (is_out ? out_text_ : err_text_).append(chunk, static_cast<std::size_t>(n));
    }
    return true;
  }

  pid_t pid_{-1};
  int status_{0};
  cmdatl::ScopedFd out_;
  cmdatl::ScopedFd err_;
};

/** @brief What one command answered. */
struct Answer {
  /** @brief The exit code, or -1 when it did not exit by itself. */
  int exit_{-1};
  /** @brief The reply, parsed, or empty when stdout held none. */
  json::object reply_;
  /** @brief stdout. */
  std::string out_;
  /** @brief stderr. */
  std::string err_;

  /**
   * @brief The error code, or empty for a success.
   * @return The code.
   */
  std::string code() const {
    const auto* error = reply_.if_contains("error");
    return error == nullptr ? std::string() : std::string(error->as_object().at("code").as_string());
  }

  /**
   * @brief A string field of the reply, or of its `error` object when the reply has none.
   * @param name The field.
   * @return Its text, or empty.
   */
  std::string text(const std::string& name) const {
    for (const auto* object : {&reply_, reply_.if_contains("error") ? &reply_.at("error").as_object() : nullptr}) {
      if (object != nullptr) {
        if (const auto* value = object->if_contains(name); value != nullptr && value->is_string()) {
          return std::string(value->as_string());
        }
      }
    }
    return {};
  }
};

/**
 * @brief A KIS account in a scratch directory, its servers and stand-in broker: demo (two servers) or real (one).
 *
 * The operator's file, the state, the cache (tokens and the calendar) and the logins all live in the scratch
 * directory; a child gets them through its environment, which holds nothing else.
 */
class KisWorld {
public:
  /**
   * @brief Starts the servers and writes the operator's file and the logins.
   * @param demo Whether a demo account.
   */
  explicit KisWorld(bool demo) : demo_(demo), day_(trading_morning()), clock_(day_) {
    trading_ = std::make_unique<support::LocalHttpsServer>();
    if (demo_) {
      market_ = std::make_unique<support::LocalHttpsServer>();
    }
    std::filesystem::create_directories(dir_.path() / "logins");
    std::filesystem::create_directories(dir_.path() / "home");
    support::spit(dir_.path() / "logins" / "kis-demo", DEMO_LOGIN);
    support::spit(dir_.path() / "logins" / "kis-real", REAL_LOGIN);
    write_config();
    script_defaults();
  }

  KisWorld(const KisWorld&) = delete;
  KisWorld& operator=(const KisWorld&) = delete;

  /** @brief Stops the servers before the broker their scripts read goes. */
  ~KisWorld() {
    trading_->stop();
    if (market_) {
      market_->stop();
    }
  }

  /** @brief Whether a demo account. */
  bool demo() const noexcept {
    return demo_;
  }

  /** @brief The server behind the account's client. */
  support::LocalHttpsServer& trading() {
    return *trading_;
  }

  /** @brief The server behind the market-data client: the trading one on a real account. */
  support::LocalHttpsServer& market() {
    return market_ ? *market_ : *trading_;
  }

  /** @brief The stand-in broker. */
  Broker& broker() {
    return broker_;
  }

  /** @brief The live clock's next start. */
  cmdatl::Clock::time_point clock() const noexcept {
    return clock_;
  }

  /**
   * @brief Moves the live clock's next start.
   * @param by How far.
   */
  void advance(std::chrono::milliseconds by) {
    clock_ += by;
  }

  /**
   * @brief Writes the operator's file.
   * @param extra Lines added to `[limits]`.
   */
  void write_config(const std::string& extra = "") const {
    support::spit(
      config(),
      std::string("[account]\nbroker = \"kis\"\nenv = \"") + (demo_ ? "demo" : "real") +
        "\"\n\n[limits]\nsymbols = [\"005930\"]\nposition = { \"005930\" = 100 }\norder_value = 10000000\n"
        "daily_orders = 20\n" +
        extra
    );
  }

  /** @brief Creates the armed file. */
  void arm() const {
    support::spit(dir_.path() / "kis.armed", "");
  }

  /** @brief The operator's file. */
  std::filesystem::path config() const {
    return dir_.path() / "kis.toml";
  }

  /** @brief The scratch directory. */
  const std::filesystem::path& dir() const noexcept {
    return dir_.path();
  }

  /** @brief The account's state directory. */
  std::filesystem::path account() const {
    return dir_.path() / "state" / "cmdatl" / "accounts" /
      (demo_ ? std::string("kis-demo-12345678-01") : std::string("kis-real-87654321-01"));
  }

  /** @brief The journal's lines, parsed. */
  std::vector<json::object> journal() const {
    std::vector<json::object> out;
    for (const auto& line : support::lines_of(support::slurp(account() / "journal.jsonl"))) {
      out.push_back(json::parse(line).as_object());
    }
    return out;
  }

  /** @brief The plan book, parsed, or empty when there is none. */
  json::object plans() const {
    const auto text = support::slurp(account() / "plans.json");
    return text.empty() ? json::object() : json::parse(text).as_object();
  }

  /**
   * @brief Starts a command as a child, its live clock at the next start, which then moves on by two seconds.
   * @param args The command line, without `--json`.
   * @return The child.
   */
  std::unique_ptr<Child> start(const std::vector<std::string>& args) {
    {
      std::lock_guard<std::mutex> lock(broker_.mutex_);
      broker_.now_ = clock_;
    }
    std::vector<std::string> argv{
      CMDATL_KIS_COMMAND,
      std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(clock_.time_since_epoch()).count()),
      trading_->port(),
      market().port(),
      (dir_.path() / "logins").string(),
      "--",
      "--json",
    };
    argv.insert(argv.end(), args.begin(), args.end());
    const std::vector<std::string> env{
      "CMDATL_CONFIG=" + config().string(),
      "XDG_STATE_HOME=" + (dir_.path() / "state").string(),
      "XDG_CACHE_HOME=" + (dir_.path() / "cache").string(),
      "XDG_CONFIG_HOME=" + (dir_.path() / "config").string(),
      "HOME=" + (dir_.path() / "home").string(),
    };
    clock_ += std::chrono::seconds(2);
    return std::make_unique<Child>(argv, env);
  }

  /**
   * @brief Runs a command to its end.
   * @param args The command line, without `--json`.
   * @return What it answered.
   */
  Answer run(const std::vector<std::string>& args) {
    auto child = start(args);
    return answer_of(*child, child->wait());
  }

  /**
   * @brief What a finished child answered.
   * @param child The child.
   * @param status Its wait status.
   * @return The answer.
   */
  static Answer answer_of(const Child& child, int status) {
    Answer answer;
    answer.exit_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    answer.out_ = child.out_text_;
    answer.err_ = child.err_text_;
    const auto lines = support::lines_of(answer.out_);
    if (!lines.empty()) {
      boost::system::error_code ec;
      auto value = json::parse(lines.back(), ec);
      if (!ec && value.is_object()) {
        answer.reply_ = std::move(value.as_object());
      }
    }
    for (const auto* secret : {"MADE-UP-DEMO-SECRET", "MADE-UP-REAL-SECRET"}) {
      CHECK(answer.out_.find(secret) == std::string::npos);
      CHECK(answer.err_.find(secret) == std::string::npos);
    }
    return answer;
  }

  /**
   * @brief The requests both servers read, in arrival order: a request goes only after the one before it was
   *   answered, so the order of arrival across the two servers is the order the command made them.
   * @return Each as `<last part of the path> <tr_id>`, the server first (`T` or `M`) on a demo account.
   */
  std::vector<std::string> sequence() const {
    std::vector<std::pair<Recorded, char>> all;
    for (auto& request : trading_->requests()) {
      all.emplace_back(std::move(request), 'T');
    }
    if (market_) {
      for (auto& request : market_->requests()) {
        all.emplace_back(std::move(request), 'M');
      }
    }
    std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) {
      return a.first.at_ < b.first.at_;
    });
    std::vector<std::string> out;
    for (const auto& [request, server] : all) {
      const auto name = request.path_.substr(request.path_.rfind('/') + 1);
      out.push_back((demo_ ? std::string(1, server) + " " : std::string()) + name + " " + request.header("tr_id"));
    }
    return out;
  }

  /** @brief Forgets the requests both servers recorded. */
  void clear() {
    trading_->clear_requests();
    if (market_) {
      market_->clear_requests();
    }
  }

  /**
   * @brief How many requests for a path both servers read.
   * @param path The path.
   * @return The count.
   */
  std::size_t count(const std::string& path) const {
    return trading_->requests(path).size() + (market_ ? market_->requests(path).size() : 0);
  }

  /**
   * @brief Sets the live clock's next start.
   * @param at The instant.
   */
  void set_clock(cmdatl::Clock::time_point at) {
    clock_ = at;
  }

  /**
   * @brief The live clock's first day, 10:00 KST.
   * @return The instant.
   */
  cmdatl::Clock::time_point day() const noexcept {
    return day_;
  }

  /**
   * @brief Plans a target on 005930, which must succeed.
   * @param target The target.
   * @param limit The limit.
   * @return The plan id.
   */
  std::string plan_id(const std::string& target, const std::string& limit = "70000") {
    const auto plan = run({"order", "plan", "005930", "--target", target, "--limit", limit});
    INFO(plan.out_ << plan.err_);
    REQUIRE(plan.exit_ == 0);
    REQUIRE(!plan.text("plan_id").empty());
    return plan.text("plan_id");
  }

  /**
   * @brief Plans and places a target on 005930, which must be placed.
   * @param target The target.
   * @param limit The limit.
   * @return The broker's order number.
   */
  std::string placed(const std::string& target, const std::string& limit = "70000") {
    const auto id = plan_id(target, limit);
    const auto place = run({"order", "place", id});
    INFO(place.out_ << place.err_);
    REQUIRE(place.exit_ == 0);
    REQUIRE(place.text("reason") == "placed");
    return std::string(place.reply_.at("order").as_object().at("order_id").as_string());
  }

  /**
   * @brief The journal's lines after its header, each as `<action> <outcome>`.
   * @return The lines.
   */
  std::vector<std::string> outcomes() const {
    std::vector<std::string> out;
    for (const auto& line : journal()) {
      if (line.contains("action")) {
        out.push_back(std::string(line.at("action").as_string()) + " " + std::string(line.at("outcome").as_string()));
      }
    }
    return out;
  }

  /** @brief On a demo account, checks that no request went to the wrong server. */
  void check_routes() const {
    if (!market_) {
      return;
    }
    for (const auto& request : trading_->requests()) {
      INFO("trading server read " << request.path_);
      CHECK(std::find(MARKET_PATHS.begin(), MARKET_PATHS.end(), request.path_) == MARKET_PATHS.end());
    }
    for (const auto& request : market_->requests()) {
      INFO("market server read " << request.path_);
      CHECK(std::find(ACCOUNT_PATHS.begin(), ACCOUNT_PATHS.end(), request.path_) == ACCOUNT_PATHS.end());
    }
  }

private:
  /** @brief Scripts each path on the server that answers it, from the stand-in broker. */
  void script_defaults() {
    const std::string token = R"({"access_token":"made-up-token","token_type":"Bearer","expires_in":86400})";
    trading_->script(TOKEN, 200, token);
    market().script(TOKEN, 200, token);
    market().script(HOLIDAY, 200, holidays(day_));
    market().script(
      PRICE, 200,
      "{" + OK_HEAD +
        R"(,"output":{"stck_shrn_iscd":"005930","stck_prpr":"70000","stck_mxpr":"91000",)"
        R"("stck_llam":"49000","aspr_unit":"100"}})"
    );
    market().script(
      REFERENCE, 200,
      "{" + OK_HEAD +
        R"(,"output":{"pdno":"00000A005930","prdt_type_cd":"300","mket_id_cd":"STK",)"
        R"("scty_grp_id_cd":"ST","std_pdno":"KR7005930003","lstg_stqt":"5969782550","scts_mket_lstg_dt":"19750611",)"
        R"("scts_mket_lstg_abol_dt":"","kosdaq_mket_lstg_dt":"","prdt_name":"Samsung","prdt_eng_name":"Samsung"}})"
    );
    if (demo_) {
      // As KIS's demo host answers reference data and the calendar (labs/FINDINGS.md F23).
      const std::string unsupported = R"({"rt_cd":"1","msg_cd":"EGW02006","msg1":"모의투자 TR 이 아닙니다."})";
      trading_->script(REFERENCE, 500, unsupported);
      trading_->script(HOLIDAY, 500, unsupported);
    }
    trading_->respond(BALANCE, [this](const Recorded&, std::size_t) {
      return Scripted{200, broker_.balance(), {}};
    });
    trading_->script(
      POWER, 200, "{" + OK_HEAD + R"(,"output":{"nrcvb_buy_amt":"100000000","max_buy_amt":"100000000"}})"
    );
    trading_->respond(AMENDABLE, [this](const Recorded&, std::size_t) {
      return Scripted{200, broker_.amendable(), {}};
    });
    trading_->respond(DAILY, [this](const Recorded& request, std::size_t) {
      return Scripted{200, broker_.daily(request), {}};
    });
    trading_->respond(ORDER, [this](const Recorded& request, std::size_t) {
      return broker_.accept(request);
    });
    trading_->respond(AMEND, [this](const Recorded& request, std::size_t) {
      return broker_.cancel(request);
    });
  }

  bool demo_;
  cmdatl::Clock::time_point day_;
  cmdatl::Clock::time_point clock_;
  support::ScratchDir dir_;
  Broker broker_;
  std::unique_ptr<support::LocalHttpsServer> trading_;
  std::unique_ptr<support::LocalHttpsServer> market_;
};

/**
 * @brief The requests a successful `order place` makes when the token and the calendar are already stored: the
 *   check before the plan's use, the snapshot, once()'s reads, the boundary's quote, the order, the reply's listing.
 * @param demo Whether a demo account.
 * @param buy Whether a buy.
 * @return The sequence, as `KisWorld::sequence` writes it.
 */
std::vector<std::string> place_sequence(bool demo, bool buy) {
  const std::string t = demo ? "T " : "";
  const std::string m = demo ? "M " : "";
  const std::string v = demo ? "V" : "T";
  const std::string list = demo ? "inquire-daily-ccld VTTC0081R" : "inquire-psbl-rvsecncl TTTC0084R";
  const std::string balance = t + "inquire-balance " + v + "TTC8434R";
  const std::string power = t + "inquire-psbl-order " + v + "TTC8908R";
  const std::string price = m + "inquire-price FHKST01010100";
  return {
    balance,
    balance,
    t + list,
    power,
    m + "search-stock-info CTPF1002R",
    t + list,
    balance,
    power,
    price,
    price,
    price,
    t + "order-cash " + v + "TTC00" + (buy ? "12U" : "11U"),
    t + list,
  };
}

/**
 * @brief The requests a successful `order cancel` makes when the token and the calendar are already stored.
 * @param demo Whether a demo account.
 * @return The sequence, as `KisWorld::sequence` writes it.
 */
std::vector<std::string> cancel_sequence(bool demo) {
  const std::string t = demo ? "T " : "";
  const std::string m = demo ? "M " : "";
  const std::string v = demo ? "V" : "T";
  const std::string list = t + (demo ? "inquire-daily-ccld VTTC0081R" : "inquire-psbl-rvsecncl TTTC0084R");
  const std::string price = m + "inquire-price FHKST01010100";
  return {
    m + "search-stock-info CTPF1002R",
    list,
    t + "inquire-balance " + v + "TTC8434R",
    t + "inquire-psbl-order " + v + "TTC8908R",
    price,
    price,
    t + "order-rvsecncl " + v + "TTC0013U",
    list,
  };
}

/**
 * @brief A request's JSON body.
 * @param request The request.
 * @return The object.
 */
json::object body_of(const Recorded& request) {
  return json::parse(request.body_).as_object();
}

/**
 * @brief A string field of a JSON object.
 * @param object The object.
 * @param name The field.
 * @return Its text, or `<absent>`.
 */
std::string field(const json::object& object, const std::string& name) {
  const auto* value = object.if_contains(name);
  return value == nullptr || !value->is_string() ? std::string("<absent>") : std::string(value->as_string());
}

/**
 * @brief The states of a reply's owed rows.
 * @param answer The reply.
 * @return Each row as `<state> <order id or unnamed>`.
 */
std::vector<std::string> owed_of(const Answer& answer) {
  std::vector<std::string> out;
  const auto* owed = answer.reply_.if_contains("owed");
  if (owed == nullptr || !owed->is_array()) {
    return out;
  }
  for (const auto& row : owed->as_array()) {
    const auto& object = row.as_object();
    const auto* id = object.if_contains("order_id");
    out.push_back(
      std::string(object.at("state").as_string()) + " " +
      (id != nullptr && id->is_string() ? std::string(id->as_string()) : std::string("unnamed"))
    );
  }
  return out;
}

/** @brief A token valid long enough. */
const std::string TOKEN_REPLY = R"({"access_token":"made-up-token","token_type":"Bearer","expires_in":86400})";

/** @brief KIS's refusal of a token request it was asked for too soon. */
const std::string TOKEN_REFUSED =
  R"json({"error_description":"접근토큰 발급 잠시 후 다시 시도하세요(1분당 1회)","error_code":"EGW00133"})json";

/**
 * @brief Kills a command with SIGKILL while it waits for a held reply, reaps it, lets the reply go (into a closed
 *   connection), and waits until the server has answered every request it read, so that the next command never runs
 *   beside a script still answering.
 * @param child The command.
 * @param server The server holding the reply.
 * @param before_release Runs after the kill and before the release, when set.
 */
void kill_held(Child& child, support::LocalHttpsServer& server, const std::function<void()>& before_release = {}) {
  child.kill();
  if (before_release) {
    before_release();
  }
  server.release();
  CHECK(server.wait_answered(std::chrono::seconds(10)));
}

/**
 * @brief Runs a command, holds the server's reply to a named occurrence of a path, and kills the command while it
 *   waits for that reply (`kill_held`).
 * @param world The account.
 * @param server The server.
 * @param path The path.
 * @param occurrence Which request for it, counted from this call.
 * @param args The command line.
 * @param fragment Text the request's query must hold, or empty.
 * @param before_release Runs after the kill and before the release, when set.
 */
void kill_at(
  KisWorld& world, support::LocalHttpsServer& server, const std::string& path, std::size_t occurrence,
  const std::vector<std::string>& args, const std::string& fragment = "",
  const std::function<void()>& before_release = {}
) {
  server.hold(path, occurrence, fragment);
  auto child = world.start(args);
  const bool held = server.wait_held(std::chrono::seconds(20));
  kill_held(*child, server, before_release);
  INFO(child->out_text_ << child->err_text_);
  REQUIRE(held);
  CHECK(child->out_text_.empty());
}

/**
 * @brief Runs a section's body on a demo account and on a real one.
 * @param body The body.
 */
void on_both(const std::function<void(bool)>& body) {
  SECTION("demo") {
    body(true);
  }
  SECTION("real") {
    body(false);
  }
}

} // namespace

TEST_CASE("B1 a buy and its sale are placed on the trading session with the market data from the market session") {
  on_both([](bool demo) {
    KisWorld world(demo);
    world.arm();
    const auto id = world.plan_id("1");
    world.clear();
    const auto place = world.run({"order", "place", id});
    INFO(place.out_ << place.err_);
    REQUIRE(place.exit_ == 0);
    CHECK(place.text("reason") == "placed");
    const auto order_id = std::string(place.reply_.at("order").as_object().at("order_id").as_string());
    CHECK(order_id == "0000010001");
    CHECK(world.sequence() == place_sequence(demo, true));
    const auto orders = world.trading().requests(ORDER);
    REQUIRE(orders.size() == 1);
    CHECK(orders.front().header("tr_id") == (demo ? "VTTC0012U" : "TTTC0012U"));
    const auto body = body_of(orders.front());
    CHECK(field(body, "CANO") == (demo ? "12345678" : "87654321"));
    CHECK(field(body, "ACNT_PRDT_CD") == "01");
    CHECK(field(body, "PDNO") == "005930");
    CHECK(field(body, "ORD_DVSN") == "00");
    CHECK(field(body, "ORD_QTY") == "1");
    CHECK(field(body, "ORD_UNPR") == "70000");
    // Neither body names the exchange: demo omits it always, and a real order from cmdatl's executor has no venue
    // (KIS proceeds at KRX for an omitted field); the journal's route says KRX.
    CHECK(field(body, "EXCG_ID_DVSN_CD") == "<absent>");
    CHECK(field(world.journal().back(), "route") == "KRX");
    if (demo) {
      // KIS's client writes a demo order on a connection no request has used.
      CHECK(orders.front().position_ == 1);
      CHECK(world.trading().requests(REFERENCE).empty());
    }
    world.check_routes();
    CHECK(world.outcomes() == std::vector<std::string>{"place pending", "place accepted"});
    CHECK(owed_of(place) == std::vector<std::string>{"named 0000010001"});

    world.broker().fill(order_id, 1);
    const auto bought = world.run({"order", "reconcile"});
    INFO(bought.out_);
    CHECK(bought.exit_ == 0);
    CHECK(bought.text("reason") == "clean");
    CHECK(owed_of(bought).empty());
    CHECK(world.outcomes().back() == "place confirmed");

    const auto sale_id = world.plan_id("0");
    world.clear();
    const auto sale = world.run({"order", "place", sale_id});
    INFO(sale.out_ << sale.err_);
    REQUIRE(sale.exit_ == 0);
    CHECK(sale.text("reason") == "placed");
    CHECK(world.sequence() == place_sequence(demo, false));
    REQUIRE(world.trading().requests(ORDER).size() == 1);
    CHECK(field(body_of(world.trading().requests(ORDER).front()), "ORD_QTY") == "1");
    world.broker().fill(std::string(sale.reply_.at("order").as_object().at("order_id").as_string()), 1);
    const auto sold = world.run({"order", "reconcile"});
    CHECK(sold.exit_ == 0);
    CHECK(sold.text("reason") == "clean");
    const auto status = world.run({"status"});
    CHECK(status.exit_ == 0);
    CHECK(status.reply_.at("positions").as_array().empty());
    CHECK(world.count(ORDER) == 1);
    world.check_routes();
  });
}

TEST_CASE("B2 a resting order partly filled is withdrawn with its listed number and amendable quantity") {
  on_both([](bool demo) {
    KisWorld world(demo);
    world.arm();
    const auto order_id = world.placed("10");
    world.broker().fill(order_id, 4);
    world.clear();
    const auto list = world.run({"order", "list"});
    INFO(list.out_);
    REQUIRE(list.exit_ == 0);
    const auto& rows = list.reply_.at("orders").as_array();
    REQUIRE(rows.size() == 1);
    CHECK(field(rows.front().as_object(), "quantity") == "10");
    CHECK(field(rows.front().as_object(), "filled") == "4");
    if (demo) {
      // Demo has no amendable-order inquiry: today's unfilled orders from the daily inquiry, dated by the client's
      // own clock (the system's date in Seoul), not by cmdatl's.
      const auto reads = world.trading().requests(DAILY);
      REQUIRE(reads.size() == 1);
      CHECK(reads.front().header("tr_id") == "VTTC0081R");
      CHECK(reads.front().param("CCLD_DVSN") == std::optional<std::string>("02"));
      CHECK(reads.front().param("INQR_STRT_DT") == reads.front().param("INQR_END_DT"));
      CHECK(world.trading().requests(AMENDABLE).empty());
    } else {
      REQUIRE(world.trading().requests(AMENDABLE).size() == 1);
      CHECK(world.trading().requests(AMENDABLE).front().header("tr_id") == "TTTC0084R");
      CHECK(world.trading().requests(DAILY).empty());
    }

    world.clear();
    const auto cancel = world.run({"order", "cancel", "005930"});
    INFO(cancel.out_ << cancel.err_);
    REQUIRE(cancel.exit_ == 0);
    CHECK(cancel.text("reason") == "withdrawal_sent");
    CHECK(cancel.reply_.at("withdrawals").as_array() == json::array{json::value(order_id)});
    CHECK(world.sequence() == cancel_sequence(demo));
    const auto cancels = world.trading().requests(AMEND);
    REQUIRE(cancels.size() == 1);
    CHECK(cancels.front().header("tr_id") == (demo ? "VTTC0013U" : "TTTC0013U"));
    const auto body = body_of(cancels.front());
    CHECK(field(body, "KRX_FWDG_ORD_ORGNO") == "06010");
    CHECK(field(body, "ORGN_ODNO") == order_id);
    CHECK(field(body, "RVSE_CNCL_DVSN_CD") == "02");
    CHECK(field(body, "ORD_QTY") == "6");
    CHECK(field(body, "QTY_ALL_ORD_YN") == "Y");
    // The real listing names KRX for the row, and a real cancel restates it; demo omits it.
    CHECK(field(body, "EXCG_ID_DVSN_CD") == (demo ? "<absent>" : "KRX"));
    if (demo) {
      CHECK(cancels.front().position_ == 1);
    }
    CHECK(
      world.outcomes() ==
      std::vector<std::string>{"place pending", "place accepted", "cancel pending", "cancel accepted"}
    );
    // Sent, not settled: the placement is owed until the dated history shows it ended.
    CHECK(owed_of(cancel) == std::vector<std::string>{"named " + order_id});

    const auto settled = world.run({"order", "reconcile"});
    INFO(settled.out_);
    CHECK(settled.exit_ == 0);
    CHECK(settled.text("reason") == "clean");
    CHECK(world.outcomes().back() == "place confirmed");
    CHECK(world.count(AMEND) == 1);
    world.check_routes();
  });
}

namespace {

/**
 * @brief Runs `order cancel` on 005930 and checks it refused an order the journal does not own and sent nothing.
 * @param world The account.
 */
void cancel_refused(KisWorld& world) {
  const auto before = world.count(AMEND);
  const auto cancel = world.run({"order", "cancel", "005930"});
  INFO(cancel.out_ << cancel.err_);
  CHECK(cancel.exit_ == 3);
  CHECK(cancel.code() == "refused.unjournaled_order");
  CHECK(world.count(AMEND) == before);
}

} // namespace

TEST_CASE("B3 a listed order the journal does not own by its positive evidence is never withdrawn") {
  on_both([](bool demo) {
    SECTION("an order placed elsewhere") {
      KisWorld world(demo);
      world.arm();
      BrokerOrder outside;
      outside.odno_ = "0000020001";
      outside.date_ = compact_kst_date(world.day());
      outside.time_ = "093000";
      world.broker().add(outside);
      cancel_refused(world);
    }
    SECTION("the journal's number at another price on both withdrawal paths") {
      KisWorld world(demo);
      world.arm();
      const auto order_id = world.placed("1");
      // A plan to reach 2 that withdraws first, made while the order is the journal's.
      const auto plan = world.run({"order", "plan", "005930", "--target", "2", "--limit", "70000"});
      INFO(plan.out_);
      REQUIRE(plan.exit_ == 0);
      CHECK(field(plan.reply_, "would") == "withdraw");
      world.broker().change(order_id, [](BrokerOrder& order) {
        order.price_ = 69000;
      });
      cancel_refused(world);
      const auto place = world.run({"order", "place", plan.text("plan_id")});
      INFO(place.out_);
      CHECK(place.exit_ == 3);
      CHECK(place.code() == "refused.unjournaled_order");
      CHECK(world.count(AMEND) == 0);
      CHECK(world.count(ORDER) == 1);
    }
    SECTION("a number acknowledged yesterday carried today by another order") {
      KisWorld world(demo);
      world.arm();
      world.set_clock(world.day() - std::chrono::hours(24));
      const auto order_id = world.placed("1");
      world.set_clock(world.day());
      world.broker().change(order_id, [&world](BrokerOrder& order) {
        order.date_ = compact_kst_date(world.day());
      });
      cancel_refused(world);
    }
    SECTION("a number two placements were acknowledged with") {
      KisWorld world(demo);
      world.arm();
      const auto order_id = world.placed("1");
      world.broker().change(order_id, [](BrokerOrder& order) {
        order.ended_ = true;
      });
      const auto ended = world.run({"order", "reconcile"});
      INFO(ended.out_);
      REQUIRE(ended.text("reason") == "clean");
      {
        std::lock_guard<std::mutex> lock(world.broker().mutex_);
        world.broker().next_ = 10001;
      }
      CHECK(world.placed("1") == order_id);
      cancel_refused(world);
    }
  });
}

namespace {

/**
 * @brief Answers the history inquiry (`CCLD_DVSN=00`) in two pages: the first with a decoy row and a cursor, the
 *   second with the stand-in broker's rows or the given failure; a demo list inquiry is answered by the broker.
 * @param world The account.
 * @param second_fails The second page's failure, or nothing.
 */
void two_history_pages(KisWorld& world, std::optional<Scripted> second_fails = std::nullopt) {
  auto& broker = world.broker();
  const auto day = compact_kst_date(world.day());
  world.trading().respond(DAILY, [&broker, day, second_fails](const Recorded& request, std::size_t) {
    if (request.param("CCLD_DVSN").value_or("") == "02") {
      return Scripted{200, broker.daily(request), {}};
    }
    if (request.param("CTX_AREA_NK100").value_or("").empty()) {
      BrokerOrder decoy;
      decoy.odno_ = "0000090001";
      decoy.symbol_ = "000660";
      decoy.date_ = day;
      decoy.time_ = "090500";
      decoy.ended_ = true;
      return Scripted{
        200,
        "{" + OK_HEAD + R"(,"ctx_area_fk100":"F1","ctx_area_nk100":"N1","output1":[)" + Broker::daily_row(decoy) +
          DAILY_TAIL,
        {{"tr_cont", "M"}}
      };
    }
    if (second_fails) {
      return *second_fails;
    }
    auto last = Scripted{200, broker.daily(request), {{"tr_cont", "D"}}};
    return last;
  });
}

/** @brief A history page KIS failed. */
const Scripted PAGE_FAILED{500, R"({"rt_cd":"1","msg_cd":"OPSQ9999","msg1":"made-up failure"})", {}};

/**
 * @brief The history inquiries a server read (`CCLD_DVSN=00`).
 * @param world The account.
 * @return The requests.
 */
std::vector<Recorded> history_reads(KisWorld& world) {
  std::vector<Recorded> out;
  for (auto& request : world.trading().requests(DAILY)) {
    if (request.param("CCLD_DVSN").value_or("") == "00") {
      out.push_back(std::move(request));
    }
  }
  return out;
}

/**
 * @brief Checks a history inquiry's filters: dates, and no symbol, order, side, cursor or limit of the caller's.
 * @param request The request.
 * @param from The first date.
 * @param to The last date.
 */
void check_history_query(const Recorded& request, const std::string& from, const std::string& to) {
  INFO(request.query_);
  CHECK(request.param("INQR_STRT_DT") == std::optional<std::string>(from));
  CHECK(request.param("INQR_END_DT") == std::optional<std::string>(to));
  CHECK(request.param("PDNO") == std::optional<std::string>(""));
  CHECK(request.param("ODNO") == std::optional<std::string>(""));
  CHECK(request.param("SLL_BUY_DVSN_CD") == std::optional<std::string>("00"));
  CHECK(request.query_.find("LIMIT") == std::string::npos);
}

} // namespace

TEST_CASE("B4 the dated history is read whole and a failed second page settles and names nothing") {
  on_both([](bool demo) {
    const std::string tr_id = demo ? "VTTC0081R" : "TTTC0081R";
    SECTION("both pages are read and the second settles the order") {
      KisWorld world(demo);
      world.arm();
      const auto order_id = world.placed("1");
      world.broker().change(order_id, [](BrokerOrder& order) {
        order.ended_ = true;
      });
      two_history_pages(world);
      world.clear();
      const auto reconcile = world.run({"order", "reconcile"});
      INFO(reconcile.out_);
      CHECK(reconcile.exit_ == 0);
      CHECK(reconcile.text("reason") == "clean");
      CHECK(world.outcomes().back() == "place confirmed");
      const auto reads = history_reads(world);
      REQUIRE(reads.size() == 2);
      CHECK(reads[0].header("tr_id") == tr_id);
      CHECK(reads[0].header("tr_cont") == "");
      CHECK(reads[0].param("CTX_AREA_NK100") == std::optional<std::string>(""));
      CHECK(reads[1].header("tr_cont") == "N");
      CHECK(reads[1].param("CTX_AREA_NK100") == std::optional<std::string>("N1"));
      CHECK(reads[1].param("CTX_AREA_FK100") == std::optional<std::string>("F1"));
      // A day wider either side of the order's day, for a named order's evidence as for a sweep.
      check_history_query(
        reads[0], compact_kst_date(world.day() - std::chrono::hours(24)),
        compact_kst_date(world.day() + std::chrono::hours(24))
      );
    }
    SECTION("an acknowledged placement whose second page fails stays owed") {
      KisWorld world(demo);
      world.arm();
      const auto order_id = world.placed("1");
      world.broker().change(order_id, [](BrokerOrder& order) {
        order.ended_ = true;
      });
      two_history_pages(world, PAGE_FAILED);
      const auto reconcile = world.run({"order", "reconcile"});
      INFO(reconcile.out_);
      CHECK(reconcile.exit_ == 3);
      CHECK(reconcile.code() == "refused.order_list_unread");
      CHECK(owed_of(reconcile) == std::vector<std::string>{"named " + order_id});
      CHECK(world.outcomes().back() == "place accepted");
      CHECK(history_reads(world).size() == 2);
      two_history_pages(world);
      const auto again = world.run({"order", "reconcile"});
      CHECK(again.exit_ == 0);
      CHECK(world.outcomes().back() == "place confirmed");
    }
    SECTION("an unanswered placement whose second page fails stays unresolved") {
      KisWorld world(demo);
      world.arm();
      const auto id = world.plan_id("1");
      world.trading().script(
        ORDER, 500, R"({"rt_cd":"1","msg_cd":"EGW00201","msg1":"초당 거래건수를 초과하였습니다."})"
      );
      const auto place = world.run({"order", "place", id});
      INFO(place.out_);
      REQUIRE(place.code() == "unknown.indeterminate");
      two_history_pages(world, PAGE_FAILED);
      world.clear();
      const auto reconcile = world.run({"order", "reconcile"});
      INFO(reconcile.out_);
      CHECK(reconcile.exit_ == 4);
      CHECK(reconcile.code() == "unknown.unresolved");
      const auto reads = history_reads(world);
      REQUIRE(reads.size() == 2);
      // A sweep reads a day wider either side of the instruction's.
      check_history_query(
        reads[0], compact_kst_date(world.day() - std::chrono::hours(24)),
        compact_kst_date(world.day() + std::chrono::hours(24))
      );
      CHECK(world.outcomes() == std::vector<std::string>{"place pending", "place indeterminate"});
    }
  });
}

TEST_CASE("B5 KIS's answers to the order request map to the journal and the exit") {
  /** @brief One reply to the order request and what it must give. */
  struct Case {
    /** @brief What it is. */
    std::string name_;
    /** @brief The HTTP status. */
    unsigned status_;
    /** @brief The body from the demo server. */
    std::string demo_body_;
    /** @brief The body from the real server. */
    std::string real_body_;
    /** @brief The journal's answer line. */
    std::string outcome_;
    /** @brief The reply's code. */
    std::string code_;
  };
  const std::vector<Case> cases{
    {"a recognized business refusal", 200, R"({"rt_cd":"1","msg_cd":"40580000","msg1":"모의투자 장종료 입니다."})",
      R"({"rt_cd":"7","msg_cd":"APBK3061","msg1":"[애프터마켓] 애프터 지정가/최유리/최우선 주문만 가능합니다."})",
      "rejected", "unknown.broker_refused"},
    {"a rate error declared in HTTP 200", 200,
      R"({"rt_cd":"1","msg_cd":"EGW00201","msg1":"초당 거래건수를 초과하였습니다."})",
      R"({"rt_cd":"1","msg_cd":"EGW00201","msg1":"초당 거래건수를 초과하였습니다."})", "rejected",
      "unknown.broker_refused"},
    {"a rate error in HTTP 500", 500, R"({"rt_cd":"1","msg_cd":"EGW00201","msg1":"초당 거래건수를 초과하였습니다."})",
      R"({"rt_cd":"1","msg_cd":"EGW00201","msg1":"초당 거래건수를 초과하였습니다."})", "indeterminate",
      "unknown.indeterminate"},
    {"another 5xx", 502, "bad gateway", "bad gateway", "indeterminate", "unknown.indeterminate"},
    {"an unrecognized refusal", 200, R"({"rt_cd":"1","msg_cd":"APBK0919","msg1":"주문가능금액을 초과 했습니다."})",
      R"({"rt_cd":"1","msg_cd":"APBK0919","msg1":"주문가능금액을 초과 했습니다."})", "indeterminate",
      "unknown.indeterminate"},
    {"a token refusal in the order's reply", 200,
      R"({"rt_cd":"1","msg_cd":"EGW00123","msg1":"기간이 만료된 token 입니다."})",
      R"({"rt_cd":"1","msg_cd":"EGW00123","msg1":"기간이 만료된 token 입니다."})", "indeterminate",
      "unknown.indeterminate"},
  };
  on_both([&cases](bool demo) {
    for (const auto& c : cases) {
      INFO(c.name_);
      KisWorld world(demo);
      world.arm();
      const auto id = world.plan_id("1");
      world.trading().script(ORDER, c.status_, demo ? c.demo_body_ : c.real_body_);
      const auto place = world.run({"order", "place", id});
      INFO(place.out_ << place.err_);
      CHECK(place.exit_ == 4);
      CHECK(place.code() == c.code_);
      CHECK(world.count(ORDER) == 1);
      CHECK(world.outcomes() == std::vector<std::string>{"place pending", "place " + c.outcome_});
      if (c.outcome_ == "rejected") {
        const auto code = json::parse(demo ? c.demo_body_ : c.real_body_).as_object().at("msg_cd").as_string();
        CHECK(place.out_.find(std::string(code)) != std::string::npos);
        CHECK(owed_of(place).empty());
        const auto next = world.run({"order", "reconcile"});
        CHECK(next.exit_ == 0);
        CHECK(next.text("reason") == "clean");
      } else {
        CHECK(owed_of(place) == std::vector<std::string>{"unnamed unnamed"});
        const auto tokens = world.trading().requests(TOKEN).size();
        const auto next = world.run({"order", "reconcile"});
        CHECK(next.exit_ == 4);
        CHECK(next.code() == "unknown.unresolved");
        // A refused token is forgotten and discarded from the store: the next command asks for a new one.
        CHECK(
          world.trading().requests(TOKEN).size() ==
          tokens + (c.demo_body_.find("EGW00123") != std::string::npos ? 1 : 0)
        );
      }
      CHECK(world.count(ORDER) == 1);
    }
  });
}

TEST_CASE("B6 a token that cannot be renewed after the pending line refuses the order before it is sent") {
  on_both([](bool demo) {
    KisWorld world(demo);
    world.arm();
    const auto id = world.plan_id("1");
    // The place command finds no stored token: it is issued for three seconds past KIS's five-minute margin, and
    // the request for the next one is refused.
    std::filesystem::remove_all(world.dir() / "cache" / "cmdatl" / "tokens");
    world.trading().script_sequence(
      TOKEN,
      {Scripted{200, R"({"access_token":"short-token","token_type":"Bearer","expires_in":303})", {}},
        Scripted{403, TOKEN_REFUSED, {}}}
    );
    world.clear();
    // Held at the boundary's quote, after every read on the trading session, until the token is no longer valid.
    world.market().hold(PRICE, 3);
    auto child = world.start({"order", "place", id});
    REQUIRE(world.market().wait_held(std::chrono::seconds(20)));
    const auto issued = world.trading().requests(TOKEN);
    REQUIRE(issued.size() == 1);
    const auto until = issued.front().at_ + std::chrono::milliseconds(3500);
    while (std::chrono::steady_clock::now() < until) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    world.market().release();
    const auto place = KisWorld::answer_of(*child, child->wait());
    INFO(place.out_ << place.err_);
    CHECK(place.exit_ == 3);
    CHECK(place.code() == "refused.not_reached");
    CHECK(world.trading().requests(TOKEN).size() == 2);
    CHECK(world.count(ORDER) == 0);
    CHECK(world.outcomes() == std::vector<std::string>{"place pending", "place rejected"});
    CHECK(owed_of(place).empty());
  });
}

TEST_CASE("B7 a login that is not the operator file's account is refused before any request") {
  const auto quiet = [](KisWorld& world, const Answer& answer, const std::string& says) {
    INFO(answer.out_ << answer.err_);
    CHECK(answer.exit_ == 1);
    CHECK(answer.code() == "auth.account_mismatch");
    CHECK(answer.out_.find(says) != std::string::npos);
    CHECK(world.trading().requests().empty());
    CHECK(world.market().requests().empty());
  };
  SECTION("an operator file naming another account") {
    KisWorld world(true);
    support::spit(
      world.config(),
      "[account]\nbroker = \"kis\"\nenv = \"demo\"\nnumber = \"99999999-01\"\n\n[limits]\nsymbols = [\"005930\"]\n"
      "position = { \"005930\" = 100 }\norder_value = 10000000\ndaily_orders = 20\n"
    );
    quiet(world, world.run({"order", "cancel", "005930"}), "not the operator file's account.number");
  }
  SECTION("a demo login stored as the real one") {
    KisWorld world(true);
    support::spit(world.dir() / "logins" / "kis-real", DEMO_LOGIN);
    quiet(world, world.run({"order", "cancel", "005930"}), "the login stored as kis/real is a demo login");
  }
  SECTION("a real login stored as the demo one") {
    KisWorld world(true);
    support::spit(world.dir() / "logins" / "kis-demo", REAL_LOGIN);
    quiet(world, world.run({"order", "cancel", "005930"}), "the login is a real login");
  }
  SECTION("a demo login on a real account") {
    KisWorld world(false);
    support::spit(world.dir() / "logins" / "kis-real", DEMO_LOGIN);
    quiet(world, world.run({"order", "cancel", "005930"}), "the login is a demo login");
  }
  SECTION("two logins with one app key are two accounts") {
    KisWorld world(true);
    world.plan_id("1");
    support::spit(
      world.dir() / "logins" / "kis-demo",
      R"({"app_key":"made-up-demo-key","app_secret":"MADE-UP-DEMO-SECRET","account_number":"11112222","demo":true})"
    );
    world.plan_id("1");
    const auto accounts = world.dir() / "state" / "cmdatl" / "accounts";
    for (
      const auto& [name, scope] :
      {std::pair<std::string, std::string>{"kis-demo-12345678-01", "kis:demo:12345678-01"},
        {"kis-demo-11112222-01", "kis:demo:11112222-01"}}
    ) {
      const auto lines = support::lines_of(support::slurp(accounts / name / "journal.jsonl"));
      REQUIRE(!lines.empty());
      CHECK(field(json::parse(lines.front()).as_object(), "scope") == scope);
    }
    // One app key, one token: the second account's command took the first's.
    CHECK(world.trading().requests(TOKEN).size() == 1);
    for (const auto& request : world.trading().requests(BALANCE)) {
      CHECK((
        request.param("CANO") == std::optional<std::string>("12345678") ||
        request.param("CANO") == std::optional<std::string>("11112222")
      ));
    }
  }
}

TEST_CASE("D1 a kill after the plan is marked used and before the pending line leaves nothing owed") {
  on_both([](bool demo) {
    KisWorld world(demo);
    world.arm();
    const auto id = world.plan_id("1");
    // Held: once()'s first read after the plan book was saved, the instrument's reference data.
    kill_at(world, world.market(), REFERENCE, 1, {"order", "place", id});
    CHECK(world.plans().at("plans").as_object().at(id).as_object().at("used") == json::value(true));
    CHECK(world.outcomes().empty());
    const auto reconcile = world.run({"order", "reconcile"});
    INFO(reconcile.out_);
    CHECK(reconcile.exit_ == 0);
    CHECK(reconcile.text("reason") == "clean");
    CHECK(owed_of(reconcile).empty());
    const auto again = world.run({"order", "place", id});
    INFO(again.out_);
    CHECK(again.exit_ == 3);
    CHECK(again.code() == "refused.plan_used");
    CHECK(world.count(ORDER) == 0);
    CHECK(world.outcomes().empty());
  });
}

TEST_CASE("D2 a pending placement killed before its request or with it unanswered reads the same afterwards") {
  SECTION("killed at the boundary's quote: unresolved in the window and unnamed after it") {
    KisWorld world(true);
    world.arm();
    const auto id = world.plan_id("1");
    kill_at(world, world.market(), PRICE, 3, {"order", "place", id});
    CHECK(world.outcomes() == std::vector<std::string>{"place pending"});
    CHECK(world.count(ORDER) == 0);
    const auto open = world.run({"order", "reconcile"});
    INFO(open.out_);
    CHECK(open.exit_ == 4);
    CHECK(open.code() == "unknown.unresolved");
    world.advance(std::chrono::minutes(5));
    const auto closed = world.run({"order", "reconcile"});
    INFO(closed.out_);
    CHECK(closed.exit_ == 4);
    CHECK(closed.code() == "unknown.unaccounted");
    CHECK(owed_of(closed) == std::vector<std::string>{"unnamed unnamed"});
    // A later dated match still names it.
    BrokerOrder late;
    late.odno_ = "0000030001";
    late.date_ = compact_kst_date(world.day());
    late.time_ = compact_kst_time(world.day() + std::chrono::seconds(4));
    world.broker().add(late);
    const auto named = world.run({"order", "reconcile"});
    INFO(named.out_);
    CHECK(owed_of(named) == std::vector<std::string>{"named 0000030001"});
    CHECK(world.count(ORDER) == 0);
  }
  on_both([](bool demo) {
    SECTION("killed with the order request received and unanswered") {
      KisWorld world(demo);
      world.arm();
      const auto id = world.plan_id("1");
      kill_at(world, world.trading(), ORDER, 1, {"order", "place", id});
      // The reply went into a closed connection: the stand-in broker took the order.
      CHECK(world.outcomes() == std::vector<std::string>{"place pending"});
      REQUIRE(world.count(ORDER) == 1);
      CHECK(support::no_children());
      const auto named = world.run({"order", "reconcile"});
      INFO(named.out_);
      CHECK(owed_of(named) == std::vector<std::string>{"named 0000010001"});
      // Named by the sweep and resting: not settled, and not cmdatl's to withdraw on either path.
      cancel_refused(world);
      const auto plan = world.run({"order", "plan", "005930", "--target", "2", "--limit", "70000"});
      INFO(plan.out_);
      CHECK(plan.exit_ == 3);
      CHECK(plan.code() == "refused.unjournaled_order");
      world.broker().fill("0000010001", 1);
      const auto settled = world.run({"order", "reconcile"});
      INFO(settled.out_);
      CHECK(settled.exit_ == 0);
      CHECK(settled.text("reason") == "clean");
      CHECK(world.outcomes().back() == "place confirmed");
      CHECK(world.count(ORDER) == 1);
      CHECK(world.count(AMEND) == 0);
    }
  });
  SECTION("two orders that each match are ambiguous") {
    KisWorld world(true);
    world.arm();
    const auto id = world.plan_id("1");
    kill_at(world, world.trading(), ORDER, 1, {"order", "place", id});
    BrokerOrder twin;
    twin.odno_ = "0000020001";
    twin.date_ = compact_kst_date(world.day());
    twin.time_ = compact_kst_time(world.day() + std::chrono::seconds(4));
    world.broker().add(twin);
    // Inside the window two candidates leave the instruction open: unresolved, and nothing is named.
    const auto reconcile = world.run({"order", "reconcile"});
    INFO(reconcile.out_);
    CHECK(reconcile.exit_ == 4);
    CHECK(reconcile.code() == "unknown.unresolved");
    CHECK(owed_of(reconcile) == std::vector<std::string>{"unnamed unnamed"});
    CHECK(world.outcomes() == std::vector<std::string>{"place pending"});
    CHECK(world.count(ORDER) == 1);
  }
}

TEST_CASE("D3 an acknowledged placement killed before its listing is a named row of this account") {
  on_both([](bool demo) {
    const auto kill_after_answer = [demo](KisWorld& world) {
      const auto id = world.plan_id("1");
      // Held: the first read after the answer, the reply's own listing.
      kill_at(world, world.trading(), demo ? DAILY : AMENDABLE, 3, {"order", "place", id}, demo ? "CCLD_DVSN=02" : "");
      CHECK(world.outcomes() == std::vector<std::string>{"place pending", "place accepted"});
    };
    SECTION("missing from the list: awaiting then unconfirmed until holdings settle it") {
      KisWorld world(demo);
      world.arm();
      kill_after_answer(world);
      world.broker().change("0000010001", [](BrokerOrder& order) {
        order.unlisted_ = true;
      });
      const auto waiting = world.run({"order", "plan", "005930", "--target", "1", "--limit", "70000"});
      INFO(waiting.out_);
      CHECK(waiting.exit_ == 3);
      CHECK(waiting.code() == "refused.awaiting_confirmation");
      world.advance(std::chrono::minutes(11));
      const auto late = world.run({"order", "plan", "005930", "--target", "1", "--limit", "70000"});
      INFO(late.out_);
      CHECK(late.exit_ == 4);
      CHECK(late.code() == "unknown.unconfirmed");
      world.broker().fill("0000010001", 1);
      const auto settled = world.run({"order", "reconcile"});
      INFO(settled.out_);
      CHECK(settled.exit_ == 0);
      CHECK(world.outcomes().back() == "place confirmed");
      CHECK(world.count(ORDER) == 1);
    }
    SECTION("still listed: owed and this account's to cancel") {
      KisWorld world(demo);
      world.arm();
      kill_after_answer(world);
      const auto owed = world.run({"order", "reconcile"});
      INFO(owed.out_);
      CHECK(owed_of(owed) == std::vector<std::string>{"named 0000010001"});
      const auto cancel = world.run({"order", "cancel", "005930"});
      INFO(cancel.out_);
      CHECK(cancel.exit_ == 0);
      CHECK(cancel.text("reason") == "withdrawal_sent");
      CHECK(world.count(AMEND) == 1);
    }
  });
}

TEST_CASE("D4 a pending cancellation names its order and leaves the placement owed until accounted for") {
  const std::string no_effect =
    R"({"rt_cd":"0","msg_cd":"APBK0013","msg1":"주문 전송 완료 되었습니다.","output":{"KRX_FWDG_ORD_ORGNO":"06010",)"
    R"("ODNO":"0000099999","ORD_TMD":"100010"}})";
  on_both([&no_effect](bool demo) {
    SECTION("killed at the cancel request: no fill and still listed") {
      KisWorld world(demo);
      world.arm();
      const auto order_id = world.placed("10");
      // The broker never acts on the held cancel: its reply goes into a closed connection and the order rests.
      kill_at(world, world.trading(), AMEND, 1, {"order", "cancel", "005930"}, "", [&world, &no_effect] {
        world.trading().script(AMEND, 200, no_effect);
      });
      CHECK(world.outcomes() == std::vector<std::string>{"place pending", "place accepted", "cancel pending"});
      const auto reconcile = world.run({"order", "reconcile"});
      INFO(reconcile.out_);
      // The cancellation is matched by its order's number, still listed; the placement stays owed and no row is
      // added for it.
      CHECK(reconcile.exit_ == 3);
      CHECK(reconcile.code() == "refused.awaiting");
      CHECK(owed_of(reconcile) == std::vector<std::string>{"named " + order_id});
      const auto lines = world.outcomes();
      CHECK(std::count(lines.begin(), lines.end(), std::string("place pending")) == 1);
      world.trading().respond(AMEND, [&world](const Recorded& request, std::size_t) {
        return world.broker().cancel(request);
      });
      const auto cancel = world.run({"order", "cancel", "005930"});
      INFO(cancel.out_);
      CHECK(cancel.exit_ == 0);
      CHECK(cancel.text("reason") == "withdrawal_sent");
      const auto settled = world.run({"order", "reconcile"});
      INFO(settled.out_);
      CHECK(world.outcomes().back() == "place confirmed");
    }
    SECTION("killed at the cancel request: partly filled and gone from the list") {
      KisWorld world(demo);
      world.arm();
      const auto order_id = world.placed("10");
      world.broker().fill(order_id, 4);
      // The broker takes the held cancel: the order ends with 4 filled and leaves the list.
      kill_at(world, world.trading(), AMEND, 1, {"order", "cancel", "005930"});
      CHECK(world.outcomes().back() == "cancel pending");
      // An absent number is no answer inside the matching window: the cancellation stays open.
      const auto open = world.run({"order", "reconcile"});
      INFO(open.out_);
      CHECK(open.exit_ == 4);
      CHECK(open.code() == "unknown.unresolved");
      CHECK(owed_of(open) == std::vector<std::string>{"named " + order_id});
      world.advance(std::chrono::minutes(5));
      const auto reconcile = world.run({"order", "reconcile"});
      INFO(reconcile.out_);
      CHECK(reconcile.exit_ == 0);
      CHECK(owed_of(reconcile).empty());
      CHECK(world.outcomes().back() == "place confirmed");
      CHECK(world.count(AMEND) == 1);
    }
  });
  SECTION("killed at a token request between the pending line and the cancel request") {
    KisWorld world(true);
    world.arm();
    const auto order_id = world.placed("10");
    std::filesystem::remove_all(world.dir() / "cache" / "cmdatl" / "tokens");
    world.trading().script_sequence(
      TOKEN,
      {Scripted{200, R"({"access_token":"short-token","token_type":"Bearer","expires_in":303})", {}},
        Scripted{200, TOKEN_REPLY, {}}}
    );
    world.clear();
    // Held at the market session's quote, after the trading session's reads, until the token is no longer valid;
    // then held again at the token request the cancellation makes after its pending line.
    world.market().hold(PRICE, 2);
    auto child = world.start({"order", "cancel", "005930"});
    REQUIRE(world.market().wait_held(std::chrono::seconds(20)));
    const auto issued = world.trading().requests(TOKEN);
    REQUIRE(issued.size() == 1);
    const auto until = issued.front().at_ + std::chrono::milliseconds(3500);
    while (std::chrono::steady_clock::now() < until) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    world.trading().hold(TOKEN, 1);
    world.market().release();
    const bool held = world.trading().wait_held(std::chrono::seconds(20));
    kill_held(*child, world.trading());
    REQUIRE(held);
    CHECK(world.outcomes().back() == "cancel pending");
    CHECK(world.count(AMEND) == 0);
    const auto reconcile = world.run({"order", "reconcile"});
    INFO(reconcile.out_);
    CHECK(reconcile.exit_ == 3);
    CHECK(reconcile.code() == "refused.awaiting");
    CHECK(owed_of(reconcile) == std::vector<std::string>{"named " + order_id});
    CHECK(world.count(AMEND) == 0);
  }
}

TEST_CASE("D5 killed commands leave no child process and no connection behind") {
  const auto before = support::open_fds();
  {
    KisWorld world(true);
    world.arm();
    const auto id = world.plan_id("1");
    kill_at(world, world.trading(), ORDER, 1, {"order", "place", id});
    CHECK(support::no_children());
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (
      (world.trading().open_connections() > 0 || world.market().open_connections() > 0) &&
      std::chrono::steady_clock::now() < until
    ) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(world.trading().open_connections() == 0);
    CHECK(world.market().open_connections() == 0);
  }
  CHECK(support::no_children());
  CHECK(support::open_fds() == before);
}
