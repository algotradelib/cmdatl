#pragma once

#include "../account/capabilities.hpp"
#include "../account/operator.hpp"
#include "../core/reply.hpp"
#include "../paper/broker.hpp"
#include "../strategy/atlc.hpp"
#include "../system/lock.hpp"
#include "./describe.hpp"

#include <algotradelib/version>

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#ifndef CMDATL_VERSION
#define CMDATL_VERSION "0.0.0.9000"
#endif

namespace cmdatl {

/**
 * @brief Whether a path names a regular file.
 * @param path The path.
 * @return Whether it does.
 */
inline bool regular_file(const std::string& path) {
  std::error_code ec;
  return !path.empty() && std::filesystem::is_regular_file(path, ec);
}

/**
 * @brief `cmdatl env`: what cmdatl found, without secrets or the process environment.
 *
 * The operator's file is read once, and atlc is looked up in that same reading; always exit 0.
 * @return The reply.
 */
inline Reply env_reply() {
  Reply reply;
  reply.body_["cmdatl"] = CMDATL_VERSION;
  reply.body_["algotradelib"] = ATL_VERSION_STRING;
  const auto snapshot = load_operator_config();
  const auto atlc = find_atlc(&snapshot);
  const auto or_null = [](const std::string& text) { return text.empty() ? json::value(nullptr) : json::value(text); };
  const auto error_json = [](const ConfigError& error) {
    return json::object{
      {"code", error.code_},
      {"message", error.message_},
      {"line", error.line_ == 0 ? json::value(nullptr) : json::value(error.line_)},
      {"column", error.column_ == 0 ? json::value(nullptr) : json::value(error.column_)},
    };
  };
  json::object found;
  found["requested"] = or_null(atlc.requested_);
  found["path"] = or_null(atlc.path_);
  found["source"] = or_null(atlc.source_);
  found["version"] = or_null(atlc.version_);
  found["minimum"] = MINIMUM_ATLC;
  found["ok"] = !atlc.version_.empty() && at_least(atlc.version_, MINIMUM_ATLC);
  if (atlc.config_error_) {
    found["error"] = error_json(*atlc.config_error_);
  }
  reply.body_["atlc"] = std::move(found);
  json::object config;
  config["path"] = or_null(snapshot.path_);
  config["exists"] = regular_file(snapshot.path_);
  config["state"] = snapshot.state_name();
  if (snapshot.state_ != ConfigSnapshot::State::OK) {
    config["error"] = error_json(snapshot.error_);
  }
  reply.body_["config"] = std::move(config);
  if (snapshot.state_ != ConfigSnapshot::State::OK) {
    for (const char* key : {"account", "limits", "armed", "state_dir", "owner", "capabilities", "paper"}) {
      reply.body_[key] = nullptr;
    }
  } else {
    const auto& c = *snapshot.config_;
    reply.body_["account"] = json::object{{"broker", c.broker_}, {"env", c.env_}, {"number", c.number_}};
    json::object position;
    for (const auto& entry : c.limits_.position_) {
      position[entry.first] = entry.second;
    }
    json::array symbols;
    for (const auto& symbol : c.limits_.symbols_) {
      symbols.push_back(json::value(symbol));
    }
    json::array phases;
    for (const auto& phase : c.limits_.phases_) {
      phases.push_back(json::value(phase));
    }
    reply.body_["limits"] = json::object{
      {"symbols", std::move(symbols)},
      {"position", std::move(position)},
      {"order_value", c.limits_.order_value_},
      {"daily_orders", c.limits_.daily_orders_},
      {"order_type", c.limits_.order_type_},
      {"collar", decimal_text(c.limits_.collar_)},
      {"phases", std::move(phases)},
      {"plan_ttl", c.limits_.plan_ttl_},
      {"portfolio_age", c.limits_.portfolio_age_},
    };
    std::error_code ec;
    reply.body_["armed"] = json::object{{"path", c.armed_path()}, {"exists", std::filesystem::exists(c.armed_path(), ec)}};
    const auto dir = account_dir(c);
    reply.body_["state_dir"] = dir ? json::value(*dir) : json::value(nullptr);
#if defined(_WIN32)
    reply.body_["owner"] = nullptr;
#else
    const auto owner = dir ? read_owner_record(*dir) : std::nullopt;
    reply.body_["owner"] = owner ? json::value(*owner) : json::value(nullptr);
#endif
    reply.body_["capabilities"] = account_capabilities<paper::PaperClient>(true);
    const paper::Costs costs;
    reply.body_["paper"] = json::object{
      {"bars", c.paper_.bars_},
      {"start", c.paper_.start_},
      {"cash", std::to_string(c.paper_.cash_)},
      {"costs", json::object{
        {"commission", decimal_text(costs.commission_)},
        {"sell_tax", decimal_text(costs.sell_tax_)},
        {"slippage_ticks", costs.slippage_ticks_},
      }},
    };
  }
  reply.body_["schema_version"] = SCHEMA_VERSION;
  reply.body_["modes"] = json::array{"paper"};
  return reply;
}

}  // namespace cmdatl
