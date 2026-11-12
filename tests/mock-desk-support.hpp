#pragma once

// A mock desk opened in process on the account a `PaperFixture` names, with the account's lock held.

#include "./paper-support.hpp"

#include <cmdatl/account/mock_desk.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

namespace cmdatl {

/** @brief The tests' way to a desk key. */
struct MockDeskTest {
  /**
   * @brief A key.
   * @return It.
   */
  static MockDeskKey key() {
    return MockDeskKey{};
  }
};

} // namespace cmdatl

namespace support {

/** @brief The fixture's account directory with its lock held, and the operator's file and bars read. */
struct HeldAccount {
  /** @brief The operator's file. */
  cmdatl::ConfigSnapshot snapshot_;
  /** @brief The bars. */
  cmdatl::MarketData data_;
  /** @brief The lock. */
  std::unique_ptr<cmdatl::OwnerLock> lock_;

  /**
   * @brief Takes the lock of the account the fixture names.
   * @param fixture The account.
   */
  explicit HeldAccount(const PaperFixture& fixture) : snapshot_(cmdatl::load_operator_config()) {
    REQUIRE(snapshot_.config_);
    REQUIRE(!cmdatl::parse_bars(slurp(fixture.dir() / "bars.csv"), data_));
    std::filesystem::create_directories(fixture.account());
    lock_ = std::make_unique<cmdatl::OwnerLock>(fixture.account().string());
    REQUIRE(
      lock_->acquire(
        std::chrono::milliseconds(100),
        [] {
          return false;
        },
        [](const std::string&) {}
      ) == cmdatl::OwnerLock::Outcome::HELD
    );
  }

  /**
   * @brief Opens a desk on the account.
   * @return The desk, its state file opened.
   */
  std::unique_ptr<cmdatl::MockDesk> desk() const {
    auto desk = std::make_unique<cmdatl::MockDesk>(cmdatl::MockDeskTest::key(), *snapshot_.config_, *lock_, data_);
    INFO(desk->rejection());
    REQUIRE(desk->opened());
    return desk;
  }
};

/**
 * @brief The state file of the account the fixture names.
 * @param fixture The account.
 * @return The path.
 */
inline std::filesystem::path state_file(const PaperFixture& fixture) {
  return fixture.account() / cmdatl::MOCK_STATE_FILE;
}

} // namespace support
