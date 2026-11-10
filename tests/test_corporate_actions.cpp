#include "cppquant/backtest.hpp"
#include "test_framework.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace cppquant;

namespace {

Bar day_bar(std::int64_t day, double price) {
  return Bar{day * 86'400, price, price + 1.0, price - 1.0, price, 1'000'000.0};
}

std::filesystem::path temporary_path(const std::string& name) {
  return std::filesystem::temp_directory_path() / name;
}

class ShortOnceStrategy : public Strategy {
public:
  explicit ShortOnceStrategy(double quantity) : quantity_(quantity) {}

  void on_bar(const Bar&, Backtester& engine) override {
    if (!submitted_) {
      engine.sell(quantity_);
      submitted_ = true;
    }
  }

private:
  double quantity_{};
  bool submitted_{};
};

class LimitOnceStrategy : public Strategy {
public:
  LimitOnceStrategy(double quantity, double limit)
      : quantity_(quantity), limit_(limit) {}

  void on_bar(const Bar&, Backtester& engine) override {
    if (!submitted_) {
      engine.buy_limit(quantity_, limit_);
      submitted_ = true;
    }
  }

private:
  double quantity_{};
  double limit_{};
  bool submitted_{};
};

class BatchBuyOnceStrategy : public BatchStrategy {
public:
  explicit BatchBuyOnceStrategy(double quantity) : quantity_(quantity) {}

  void on_batch(const BarBatch&, Backtester& engine) override {
    if (!submitted_) {
      engine.buy("AAA", quantity_);
      submitted_ = true;
    }
  }

private:
  double quantity_{};
  bool submitted_{};
};

void zero_cost(BacktestConfig& config) {
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  config.max_volume_participation = 1.0;
}

} // namespace

TEST(split_scales_position_and_average_cost) {
  const std::vector<Bar> bars{day_bar(0, 100.0), day_bar(1, 100.0),
                              day_bar(2, 50.0), day_bar(3, 50.0)};

  BacktestConfig config;
  zero_cost(config);
  config.initial_cash = 100'000.0;

  Backtester engine(config);
  BuyAndHoldStrategy strategy(100.0);
  const CorporateAction split{2 * 86'400, "AAPL", CorporateActionType::Split,
                              2.0};
  const Performance result = engine.run(bars, strategy, {split});

  CHECK_NEAR(engine.position(), 200.0, 1e-12);
  CHECK_NEAR(engine.average_cost(), 50.0, 1e-12);
  CHECK_NEAR(result.final_equity, 100'000.0, 1e-9);
  CHECK_NEAR(result.total_return, 0.0, 1e-12);
}

TEST(split_adjusts_pending_limit_orders) {
  const std::vector<Bar> bars{
      day_bar(0, 100.0), day_bar(1, 100.0),
      Bar{2 * 86'400, 50.0, 51.0, 40.0, 50.0, 1'000'000.0}};

  BacktestConfig config;
  zero_cost(config);
  config.initial_cash = 100'000.0;

  Backtester engine(config);
  LimitOnceStrategy strategy(10.0, 90.0);
  const CorporateAction split{2 * 86'400, "", CorporateActionType::Split, 2.0};
  engine.run(bars, strategy, {split});

  CHECK_EQ(engine.fills().size(), 1u);
  CHECK_NEAR(engine.fills()[0].price, 45.0, 1e-12);
  CHECK_NEAR(engine.fills()[0].quantity, 20.0, 1e-12);
  CHECK_NEAR(engine.position(), 20.0, 1e-12);
  CHECK_NEAR(engine.average_cost(), 45.0, 1e-12);
}

TEST(cash_dividend_credits_long_and_debits_short) {
  const std::vector<Bar> bars{day_bar(0, 100.0), day_bar(1, 100.0),
                              day_bar(2, 100.0)};
  const CorporateAction dividend{2 * 86'400, "",
                                 CorporateActionType::CashDividend, 2.0};

  BacktestConfig config;
  zero_cost(config);
  config.initial_cash = 100'000.0;

  Backtester long_engine(config);
  BuyAndHoldStrategy long_strategy(100.0);
  const Performance long_result =
      long_engine.run(bars, long_strategy, {dividend});
  CHECK_NEAR(long_result.final_equity, 100'200.0, 1e-9);
  CHECK_NEAR(long_result.total_dividends, 200.0, 1e-12);

  Backtester short_engine(config);
  ShortOnceStrategy short_strategy(100.0);
  const Performance short_result =
      short_engine.run(bars, short_strategy, {dividend});
  CHECK_NEAR(short_result.final_equity, 99'800.0, 1e-9);
  CHECK_NEAR(short_result.total_dividends, -200.0, 1e-12);
}

TEST(margin_cost_accrues_on_negative_cash) {
  const std::vector<Bar> bars{day_bar(0, 100.0), day_bar(1, 100.0),
                              day_bar(2, 100.0)};
  BacktestConfig config;
  zero_cost(config);
  config.initial_cash = 1'000.0;
  config.margin_rate = 0.10;

  Backtester engine(config);
  BuyAndHoldStrategy strategy(100.0);
  const Performance result = engine.run(bars, strategy);

  const double expected = 9'000.0 * 0.10 / 365.0;
  CHECK_NEAR(result.total_margin_cost, expected, 1e-9);
  CHECK_NEAR(result.final_equity, 1'000.0 - expected, 1e-9);
}

TEST(borrow_cost_accrues_on_short_positions) {
  const std::vector<Bar> bars{day_bar(0, 100.0), day_bar(1, 100.0),
                              day_bar(2, 100.0)};
  BacktestConfig config;
  zero_cost(config);
  config.initial_cash = 100'000.0;
  config.borrow_rate = 0.05;

  Backtester engine(config);
  ShortOnceStrategy strategy(100.0);
  const Performance result = engine.run(bars, strategy);

  const double expected = 100.0 * 100.0 * 0.05 / 365.0;
  CHECK_NEAR(result.total_borrow_cost, expected, 1e-9);
  CHECK_NEAR(result.final_equity, 100'000.0 - expected, 1e-9);
}

TEST(batch_split_applies_to_the_matching_symbol) {
  const BarBatch first{
      0, {{"AAA", day_bar(0, 100.0)}, {"BBB", day_bar(0, 100.0)}}};
  const BarBatch second{
      86'400, {{"AAA", day_bar(1, 100.0)}, {"BBB", day_bar(1, 100.0)}}};
  const BarBatch third{2 * 86'400,
                       {{"AAA", day_bar(2, 50.0)}, {"BBB", day_bar(2, 100.0)}}};
  const std::vector<BarBatch> batches{first, second, third};

  BacktestConfig config;
  zero_cost(config);
  config.initial_cash = 100'000.0;
  Backtester engine(config);
  BatchBuyOnceStrategy strategy(100.0);
  const CorporateAction split{2 * 86'400, "AAA", CorporateActionType::Split,
                              2.0};
  const Performance result =
      engine.run_batch(batches, "AAA", strategy, {split});

  CHECK_NEAR(engine.position("AAA"), 200.0, 1e-12);
  CHECK_NEAR(engine.average_cost("AAA"), 50.0, 1e-12);
  CHECK_NEAR(engine.position("BBB"), 0.0, 1e-12);
  CHECK_NEAR(result.final_equity, 100'000.0, 1e-9);
}

TEST(batch_corporate_actions_require_known_symbols) {
  const BarBatch first{0, {{"AAA", day_bar(0, 100.0)}}};
  const BarBatch second{86'400, {{"AAA", day_bar(1, 100.0)}}};
  const std::vector<BarBatch> batches{first, second};
  Backtester engine;
  BatchBuyOnceStrategy strategy(10.0);

  const CorporateAction empty_symbol{86'400, "", CorporateActionType::Split,
                                     2.0};
  CHECK_THROW(engine.run_batch(batches, "AAA", strategy, {empty_symbol}),
              std::invalid_argument);

  const CorporateAction unknown{86'400, "ZZZ", CorporateActionType::Split, 2.0};
  CHECK_THROW(engine.run_batch(batches, "AAA", strategy, {unknown}),
              std::invalid_argument);
}

TEST(corporate_action_csv_loads_and_sorts) {
  const auto path = temporary_path("cppquant_corporate_actions.csv");
  {
    std::ofstream file(path);
    file << "timestamp,symbol,type,value\n";
    file << "100,B,split,2\n";
    file << "# a comment\n";
    file << "50,A,dividend,0.25\n";
    file << "150,A,cash-dividend,0.1\n";
  }

  const auto actions = load_corporate_actions_csv(path.string());
  std::remove(path.string().c_str());

  CHECK_EQ(actions.size(), 3u);
  CHECK_EQ(actions[0].timestamp, 50);
  CHECK(actions[0].type == CorporateActionType::CashDividend);
  CHECK_NEAR(actions[0].value, 0.25, 1e-12);
  CHECK_EQ(actions[1].timestamp, 100);
  CHECK(actions[1].type == CorporateActionType::Split);
  CHECK_EQ(actions[1].symbol, "B");
  CHECK_EQ(actions[2].timestamp, 150);
  CHECK(actions[2].type == CorporateActionType::CashDividend);
}

TEST(corporate_action_validation_rejects_bad_events) {
  const std::vector<Bar> bars{day_bar(0, 100.0), day_bar(1, 100.0)};

  BacktestConfig config;
  zero_cost(config);
  Backtester engine(config);
  BuyAndHoldStrategy strategy(1.0);

  CHECK_THROW(
      engine.run(bars, strategy,
                 {CorporateAction{0, "", CorporateActionType::Split, 0.0}}),
      std::invalid_argument);
  CHECK_THROW(engine.run(bars, strategy,
                         {CorporateAction{
                             0, "", CorporateActionType::CashDividend, -1.0}}),
              std::invalid_argument);
  CHECK_THROW(engine.run(bars, strategy,
                         {CorporateAction{0, "", CorporateActionType::Split,
                                          std::nan("")}}),
              std::invalid_argument);
}

TEST(corporate_action_csv_rejects_malformed_rows) {
  const auto path = temporary_path("cppquant_corporate_actions_bad.csv");
  {
    std::ofstream file(path);
    file << "timestamp,symbol,type,value\n";
    file << "1,A,split,0\n";
  }
  CHECK_THROW(load_corporate_actions_csv(path.string()), std::invalid_argument);
  std::remove(path.string().c_str());

  {
    std::ofstream file(path);
    file << "1,A,unknown,2\n";
  }
  CHECK_THROW(load_corporate_actions_csv(path.string()), std::runtime_error);
  std::remove(path.string().c_str());

  {
    std::ofstream file(path);
    file << "1,A,split\n";
  }
  CHECK_THROW(load_corporate_actions_csv(path.string()), std::runtime_error);
  std::remove(path.string().c_str());
}