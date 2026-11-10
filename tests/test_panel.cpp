#include "cppquant/backtest.hpp"
#include "test_framework.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace cppquant;

namespace {

Bar bar(std::int64_t timestamp, double open, double close,
        double volume = 1'000'000.0) {
  return Bar{timestamp,
             open,
             std::max(open, close) + 0.5,
             std::min(open, close) - 0.5,
             close,
             volume};
}

std::filesystem::path temporary_path(const std::string& name) {
  return std::filesystem::temp_directory_path() / name;
}

class ScriptedBatchStrategy : public BatchStrategy {
public:
  using Action = std::function<void(const BarBatch&, Backtester&, std::size_t)>;

  explicit ScriptedBatchStrategy(Action action) : action_(std::move(action)) {}

  void on_batch(const BarBatch& batch, Backtester& engine) override {
    action_(batch, engine, index_++);
  }

private:
  Action action_;
  std::size_t index_{};
};

class CrossAssetStrategy : public BatchStrategy {
public:
  explicit CrossAssetStrategy(double quantity) : quantity_(quantity) {}

  void on_batch(const BarBatch& batch, Backtester& engine) override {
    const Bar* first = batch.find("AAA");
    const Bar* second = batch.find("BBB");
    if (first == nullptr || second == nullptr) {
      return;
    }
    if (second->close > first->close) {
      engine.order_target(quantity_);
    } else {
      engine.order_target(0.0);
    }
  }

private:
  double quantity_{};
};

} // namespace

TEST(panel_aligns_the_union_of_timestamps) {
  const std::vector<SymbolSeries> series{
      SymbolSeries{
          "AAA",
          {bar(0, 100, 100), bar(86'400, 100, 105), bar(172'800, 105, 110)}},
      SymbolSeries{
          "BBB", {bar(0, 50, 50), bar(172'800, 50, 49), bar(259'200, 49, 51)}},
  };
  const BarPanel panel(series);

  CHECK_EQ(panel.symbols().size(), 2u);
  CHECK_EQ(panel.batches().size(), 4u);
  CHECK_EQ(panel.missing_bars(), 2u);
  CHECK_EQ(panel.missing_bars("AAA"), 1u);
  CHECK_EQ(panel.missing_bars("BBB"), 1u);

  CHECK_EQ(panel.batches()[0].bars.size(), 2u);
  CHECK_EQ(panel.batches()[1].bars.size(), 1u);
  CHECK(panel.batches()[1].find("AAA") != nullptr);
  CHECK(panel.batches()[1].find("BBB") == nullptr);
  CHECK_EQ(panel.batches()[2].bars.size(), 2u);
  CHECK_EQ(panel.batches()[3].bars.size(), 1u);
  CHECK(panel.batches()[3].find("BBB") != nullptr);
}

TEST(panel_rejects_invalid_series) {
  CHECK_THROW(BarPanel(std::vector<SymbolSeries>{}), std::invalid_argument);

  CHECK_THROW(BarPanel({SymbolSeries{"AAA", {}}, SymbolSeries{"BBB", {}}}),
              std::invalid_argument);

  CHECK_THROW(BarPanel({SymbolSeries{"", {bar(0, 100, 100)}}}),
              std::invalid_argument);

  CHECK_THROW(BarPanel({SymbolSeries{"AAA", {bar(0, 100, 100)}},
                        SymbolSeries{"AAA", {bar(0, 100, 100)}}}),
              std::invalid_argument);

  CHECK_THROW(BarPanel({SymbolSeries{
                  "AAA", {bar(86'400, 100, 100), bar(0, 100, 100)}}}),
              std::invalid_argument);

  CHECK_THROW(
      BarPanel({SymbolSeries{"AAA", {bar(0, 100, 100), bar(0, 100, 100)}}}),
      std::invalid_argument);

  CHECK_THROW(BarPanel({SymbolSeries{"AAA", {bar(0, -100, 100)}}}),
              std::invalid_argument);
}

TEST(multi_asset_positions_share_cash) {
  const std::vector<Bar> first = {
      bar(0, 100, 100),
      bar(86'400, 110, 110),
      bar(172'800, 120, 120),
  };
  const std::vector<Bar> second = {
      bar(0, 50, 50),
      bar(86'400, 60, 60),
      bar(172'800, 70, 70),
  };
  const BarPanel panel(
      {SymbolSeries{"AAA", first}, SymbolSeries{"BBB", second}});

  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  config.max_volume_participation = 1.0;
  Backtester engine(config);

  ScriptedBatchStrategy strategy(
      [](const BarBatch&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          backtester.buy("AAA", 10.0);
          backtester.buy("BBB", 5.0);
        } else if (index == 1) {
          backtester.sell("AAA", 10.0);
        }
      });

  const Performance result = engine.run_batch(panel.batches(), "AAA", strategy);

  CHECK_EQ(engine.fills().size(), 3u);
  CHECK_EQ(engine.fills()[0].symbol, std::string("AAA"));
  CHECK_EQ(engine.fills()[1].symbol, std::string("BBB"));
  CHECK_EQ(engine.fills()[2].symbol, std::string("AAA"));
  CHECK_NEAR(engine.position("AAA"), 0.0, 1e-12);
  CHECK_NEAR(engine.position("BBB"), 5.0, 1e-12);
  CHECK_NEAR(engine.cash(), 9'800.0, 1e-12);
  CHECK_NEAR(engine.realized_pnl("AAA"), 100.0, 1e-12);
  CHECK_NEAR(engine.realized_pnl("BBB"), 0.0, 1e-12);
  CHECK_NEAR(engine.portfolio_equity(), 10'150.0, 1e-12);
  CHECK_NEAR(result.final_equity, 10'150.0, 1e-12);
  CHECK_NEAR(result.final_position, 0.0, 1e-12);
  CHECK_EQ(result.final_positions.size(), 1u);
  CHECK_NEAR(result.final_positions.at("BBB"), 5.0, 1e-12);
}

TEST(multi_asset_target_weights_rebalance) {
  const std::vector<Bar> first = {
      bar(0, 100, 100),
      bar(86'400, 100, 100),
  };
  const std::vector<Bar> second = {
      bar(0, 50, 50),
      bar(86'400, 50, 50),
  };
  const BarPanel panel(
      {SymbolSeries{"AAA", first}, SymbolSeries{"BBB", second}});

  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  config.max_volume_participation = 1.0;
  Backtester engine(config);

  ScriptedBatchStrategy strategy(
      [](const BarBatch&, Backtester& backtester, std::size_t index) {
        if (index != 0) {
          return;
        }
        const double equity = backtester.portfolio_equity();
        backtester.order_target("AAA", 0.5 * equity / 100.0);
        backtester.order_target("BBB", 0.5 * equity / 50.0);
        backtester.order_target("AAA", 0.5 * equity / 100.0);
      });

  const Performance result = engine.run_batch(panel.batches(), "AAA", strategy);

  CHECK_EQ(engine.fills().size(), 2u);
  CHECK_NEAR(engine.position("AAA"), 50.0, 1e-12);
  CHECK_NEAR(engine.position("BBB"), 100.0, 1e-12);
  CHECK_NEAR(engine.cash(), 0.0, 1e-12);
  CHECK_NEAR(engine.portfolio_equity(), 10'000.0, 1e-12);
  CHECK_NEAR(result.final_positions.at("AAA"), 50.0, 1e-12);
  CHECK_NEAR(result.final_positions.at("BBB"), 100.0, 1e-12);
}

TEST(multi_asset_equity_uses_last_mark_for_missing_symbol) {
  const std::vector<Bar> first = {
      bar(0, 100, 100),
      bar(86'400, 100, 100),
      bar(172'800, 100, 100),
  };
  const std::vector<Bar> second = {
      bar(0, 50, 50),
      bar(86'400, 50, 60),
  };
  const BarPanel panel(
      {SymbolSeries{"AAA", first}, SymbolSeries{"BBB", second}});

  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  config.max_volume_participation = 1.0;
  Backtester engine(config);

  ScriptedBatchStrategy strategy(
      [](const BarBatch&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          backtester.buy("BBB", 5.0);
        }
      });

  engine.run_batch(panel.batches(), "AAA", strategy);

  CHECK_EQ(panel.batches().size(), 3u);
  CHECK_EQ(panel.missing_bars("BBB"), 1u);
  CHECK_NEAR(engine.position("BBB"), 5.0, 1e-12);
  CHECK_NEAR(engine.cash(), 9'750.0, 1e-12);
  CHECK_NEAR(engine.portfolio_equity(), 10'050.0, 1e-12);
}

TEST(symbol_order_target_does_not_stack_pending_orders) {
  const std::vector<Bar> first = {
      bar(0, 100, 100),
      bar(86'400, 100, 100),
  };
  const std::vector<Bar> second = {
      bar(0, 50, 50),
      bar(86'400, 50, 50),
  };
  const BarPanel panel(
      {SymbolSeries{"AAA", first}, SymbolSeries{"BBB", second}});
  Backtester engine;

  ScriptedBatchStrategy strategy(
      [](const BarBatch&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          backtester.order_target("AAA", 10.0);
          backtester.order_target("AAA", 10.0);
          backtester.order_target("BBB", 5.0);
          backtester.order_target("BBB", 5.0);
        }
      });

  engine.run_batch(panel.batches(), "AAA", strategy);

  CHECK_EQ(engine.fills().size(), 2u);
  CHECK_NEAR(engine.position("AAA"), 10.0, 1e-12);
  CHECK_NEAR(engine.position("BBB"), 5.0, 1e-12);
}

TEST(close_position_targets_one_symbol) {
  const std::vector<Bar> first = {
      bar(0, 100, 100),
      bar(86'400, 100, 100),
      bar(172'800, 110, 110),
  };
  const std::vector<Bar> second = {
      bar(0, 50, 50),
      bar(86'400, 50, 50),
      bar(172'800, 60, 60),
  };
  const BarPanel panel(
      {SymbolSeries{"AAA", first}, SymbolSeries{"BBB", second}});
  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  config.max_volume_participation = 1.0;
  Backtester engine(config);

  ScriptedBatchStrategy strategy(
      [](const BarBatch&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          backtester.buy("AAA", 10.0);
          backtester.buy("BBB", 5.0);
        } else if (index == 1) {
          backtester.close_position("AAA");
        }
      });

  engine.run_batch(panel.batches(), "AAA", strategy);

  CHECK_NEAR(engine.position("AAA"), 0.0, 1e-12);
  CHECK_NEAR(engine.position("BBB"), 5.0, 1e-12);
  CHECK_NEAR(engine.realized_pnl("AAA"), 100.0, 1e-12);
}

TEST(loader_reads_a_long_format_panel) {
  const auto path = temporary_path("cppquant_panel.csv");
  {
    std::ofstream file(path);
    file << "timestamp,symbol,open,high,low,close,volume\n";
    file << "0,AAA,100,101,99,100,1000\n";
    file << "0,BBB,50,51,49,50,1000\n";
    file << "86400,AAA,100,106,99,105,1000\n";
    file << "86400,BBB,50,50,48,49,1000\n";
    file << "172800,AAA,105,111,104,110,1000\n";
  }

  const BarPanel panel = load_bar_panel_csv(path.string());
  std::remove(path.string().c_str());

  CHECK_EQ(panel.symbols().size(), 2u);
  CHECK_EQ(panel.batches().size(), 3u);
  CHECK_EQ(panel.missing_bars(), 1u);
  CHECK_EQ(panel.missing_bars("BBB"), 1u);
  CHECK_NEAR(panel.batches()[2].find("AAA")->close, 110.0, 1e-12);
}

TEST(loader_rejects_malformed_panel_rows) {
  const auto path = temporary_path("cppquant_panel_bad.csv");
  {
    std::ofstream file(path);
    file << "timestamp,symbol,open,high,low,close,volume\n";
    file << "0,AAA,100,99,101,100,1000\n";
  }
  CHECK_THROW(load_bar_panel_csv(path.string()), std::invalid_argument);
  std::remove(path.string().c_str());

  CHECK_THROW(load_bar_panel_csv("/definitely/not/a/panel.csv"),
              std::runtime_error);
}

TEST(run_batch_matches_single_symbol_run) {
  const std::vector<Bar> bars = {
      bar(0, 100, 100),
      bar(86'400, 100, 105),
      bar(172'800, 105, 110),
  };

  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  config.max_volume_participation = 1.0;

  Backtester single(config);
  BuyAndHoldStrategy single_strategy(10.0);
  const Performance single_result = single.run(bars, single_strategy);

  const BarPanel panel({SymbolSeries{"AAA", bars}});
  Backtester batch(config);
  BuyAndHoldStrategy batch_strategy(10.0);
  SingleSymbolBatchStrategy adapter("AAA", batch_strategy);
  const Performance batch_result =
      batch.run_batch(panel.batches(), "AAA", adapter);

  CHECK_NEAR(batch_result.final_equity, single_result.final_equity, 1e-12);
  CHECK_EQ(batch_result.trades, single_result.trades);
  CHECK_EQ(batch.fills().size(), single.fills().size());
  CHECK_NEAR(batch.position(), single.position(), 1e-12);
}

TEST(batch_strategy_can_use_cross_asset_context) {
  const std::vector<Bar> first = {
      bar(0, 100, 100),
      bar(86'400, 101, 101),
      bar(172'800, 102, 102),
  };
  const std::vector<Bar> second = {
      bar(0, 110, 110),
      bar(86'400, 90, 90),
      bar(172'800, 110, 110),
  };
  const BarPanel panel(
      {SymbolSeries{"AAA", first}, SymbolSeries{"BBB", second}});

  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  config.max_volume_participation = 1.0;
  Backtester engine(config);

  CrossAssetStrategy strategy(10.0);
  const Performance result = engine.run_batch(panel.batches(), "AAA", strategy);

  CHECK_EQ(engine.fills().size(), 2u);
  CHECK_NEAR(engine.fills()[0].price, 101.0, 1e-12);
  CHECK_NEAR(engine.fills()[1].price, 102.0, 1e-12);
  CHECK_NEAR(engine.position(), 0.0, 1e-12);
  CHECK_NEAR(result.realized_pnl, 10.0, 1e-12);
}

TEST(run_batch_rejects_missing_trade_symbol) {
  const BarPanel panel({SymbolSeries{"AAA", {bar(0, 100, 100)}},
                        SymbolSeries{"BBB", {bar(0, 50, 50)}}});
  Backtester engine;
  BuyAndHoldStrategy strategy(1.0);
  SingleSymbolBatchStrategy adapter("AAA", strategy);

  CHECK_THROW(engine.run_batch(panel.batches(), "CCC", adapter),
              std::invalid_argument);
}

TEST(run_batch_rejects_invalid_batch_order_and_duplicates) {
  Backtester engine;
  BuyAndHoldStrategy strategy(1.0);
  SingleSymbolBatchStrategy adapter("AAA", strategy);

  const BarBatch first{0, {SymbolBar{"AAA", bar(0, 100, 100)}}};
  const BarBatch earlier{0, {SymbolBar{"AAA", bar(0, 100, 100)}}};
  CHECK_THROW(engine.run_batch({first, earlier}, "AAA", adapter),
              std::invalid_argument);

  const BarBatch duplicate{
      0,
      {SymbolBar{"AAA", bar(0, 100, 100)}, SymbolBar{"AAA", bar(0, 100, 100)}}};
  CHECK_THROW(engine.run_batch({duplicate}, "AAA", adapter),
              std::invalid_argument);
}

TEST(single_symbol_adapter_reports_missing_symbol) {
  Backtester engine;
  BuyAndHoldStrategy strategy(1.0);
  SingleSymbolBatchStrategy adapter("BBB", strategy);
  const BarBatch batch{0, {SymbolBar{"AAA", bar(0, 100, 100)}}};

  CHECK_THROW(engine.run_batch({batch}, "AAA", adapter), std::runtime_error);
}
