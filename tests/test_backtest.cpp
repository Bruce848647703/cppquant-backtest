#include "cppquant/backtest.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

using namespace cppquant;

namespace {

Bar make_bar(std::int64_t timestamp, double open, double close,
             double volume = 1'000'000.0) {
  return Bar{timestamp,
             open,
             std::max(open, close) + 0.5,
             std::min(open, close) - 0.5,
             close,
             volume};
}

Bar make_ohlc(std::int64_t timestamp, double open, double high, double low,
              double close, double volume = 1'000'000.0) {
  return Bar{timestamp, open, high, low, close, volume};
}

class ScriptedStrategy : public Strategy {
public:
  using Action = std::function<void(const Bar&, Backtester&, std::size_t)>;

  explicit ScriptedStrategy(Action action) : action_(std::move(action)) {}

  void on_bar(const Bar& bar, Backtester& engine) override {
    action_(bar, engine, index_++);
  }

private:
  Action action_;
  std::size_t index_{};
};

std::vector<Bar> bars_from_closes(const std::vector<double>& closes) {
  std::vector<Bar> bars;
  bars.reserve(closes.size());
  std::int64_t timestamp = 1'577'836'800;
  for (const double close : closes) {
    bars.push_back(make_bar(timestamp, close, close));
    timestamp += 86'400;
  }
  return bars;
}

std::filesystem::path temporary_path(const std::string& name) {
  return std::filesystem::temp_directory_path() / name;
}

} // namespace

TEST(buy_and_hold_marks_the_position_to_market) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_bar(86'400, 100.0, 105.0),
      make_bar(172'800, 105.0, 110.0),
  };

  BacktestConfig config;
  config.initial_cash = 1'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  Backtester engine(config);

  BuyAndHoldStrategy strategy(10.0);
  const Performance performance = engine.run(bars, strategy);

  CHECK_EQ(engine.fills().size(), 1u);
  CHECK_EQ(engine.fills()[0].side, Side::Buy);
  CHECK_NEAR(engine.fills()[0].price, 100.0, 1e-12);
  CHECK_EQ(engine.fills()[0].timestamp, 86'400);
  CHECK_NEAR(engine.position(), 10.0, 1e-12);
  CHECK_NEAR(engine.cash(), 0.0, 1e-12);
  CHECK_EQ(engine.equity_curve().size(), 3u);
  CHECK_NEAR(engine.equity_curve()[0], 1'000.0, 1e-12);
  CHECK_NEAR(engine.equity_curve()[1], 1'050.0, 1e-12);
  CHECK_NEAR(engine.equity_curve()[2], 1'100.0, 1e-12);
  CHECK_NEAR(performance.final_equity, 1'100.0, 1e-12);
  CHECK_NEAR(performance.total_return, 0.10, 1e-12);
  CHECK_NEAR(performance.final_position, 10.0, 1e-12);
}

TEST(orders_execute_at_the_next_open_with_costs) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_bar(86'400, 100.0, 100.0),
  };

  BacktestConfig config;
  config.initial_cash = 1'000.0;
  config.commission_bps = 10.0; // 0.10%
  config.slippage_bps = 50.0;   // 0.50%
  Backtester engine(config);

  ScriptedStrategy strategy(
      [](const Bar&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          backtester.buy(1.0);
        }
      });

  engine.run(bars, strategy);

  CHECK_EQ(engine.fills().size(), 1u);
  CHECK_NEAR(engine.fills()[0].price, 100.5, 1e-12);
  CHECK_NEAR(engine.fills()[0].commission, 0.1005, 1e-12);
  CHECK_NEAR(engine.cash(), 899.3995, 1e-9);
  CHECK_NEAR(engine.equity(100.0), 999.3995, 1e-9);
}

TEST(short_positions_realize_profit_when_prices_fall) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_bar(86'400, 100.0, 90.0),
      make_bar(172'800, 90.0, 80.0),
  };

  BacktestConfig config;
  config.initial_cash = 1'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  Backtester engine(config);

  ScriptedStrategy strategy(
      [](const Bar&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          backtester.sell(5.0);
        } else if (index == 1) {
          backtester.buy(5.0);
        }
      });

  const Performance performance = engine.run(bars, strategy);

  CHECK_EQ(engine.fills().size(), 2u);
  CHECK_EQ(engine.fills()[0].side, Side::Sell);
  CHECK_NEAR(engine.fills()[0].price, 100.0, 1e-12);
  CHECK_NEAR(engine.fills()[1].price, 90.0, 1e-12);
  CHECK_NEAR(engine.position(), 0.0, 1e-12);
  CHECK_NEAR(engine.cash(), 1'050.0, 1e-12);
  CHECK_NEAR(engine.realized_pnl(), 50.0, 1e-12);
  CHECK_NEAR(performance.final_equity, 1'050.0, 1e-12);
  CHECK_NEAR(performance.total_return, 0.05, 1e-12);
  CHECK_EQ(performance.winning_trades, 1u);
  CHECK_EQ(performance.losing_trades, 0u);
}

TEST(partial_closes_update_average_cost_and_realized_pnl) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),       make_bar(86'400, 100.0, 100.0),
      make_bar(172'800, 110.0, 110.0), make_bar(259'200, 120.0, 120.0),
      make_bar(345'600, 120.0, 120.0),
  };

  BacktestConfig config;
  config.initial_cash = 1'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  Backtester engine(config);

  ScriptedStrategy strategy(
      [](const Bar&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          backtester.buy(10.0);
        } else if (index == 1) {
          backtester.sell(4.0);
        } else if (index == 2) {
          backtester.sell(10.0);
        }
      });

  const Performance performance = engine.run(bars, strategy);

  CHECK_EQ(engine.fills().size(), 3u);
  CHECK_NEAR(engine.realized_pnl(), 160.0, 1e-12);
  CHECK_NEAR(engine.position(), -4.0, 1e-12);
  CHECK_NEAR(engine.average_cost(), 120.0, 1e-12);
  CHECK_NEAR(engine.cash(), 1'640.0, 1e-12);
  CHECK_NEAR(performance.final_equity, engine.equity(bars.back().close), 1e-12);
  CHECK_EQ(performance.winning_trades, 2u);
}

TEST(order_target_does_not_duplicate_orders) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_bar(86'400, 100.0, 100.0),
      make_bar(172'800, 100.0, 100.0),
  };

  Backtester engine;
  ScriptedStrategy strategy([](const Bar&, Backtester& backtester,
                               std::size_t) { backtester.order_target(10.0); });

  const Performance performance = engine.run(bars, strategy);

  CHECK_EQ(engine.fills().size(), 1u);
  CHECK_NEAR(engine.position(), 10.0, 1e-12);
  CHECK_NEAR(performance.final_position, 10.0, 1e-12);
}

TEST(close_position_flattens_a_long_position) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_bar(86'400, 100.0, 105.0),
      make_bar(172'800, 105.0, 110.0),
  };

  BacktestConfig config;
  config.initial_cash = 1'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  Backtester engine(config);
  ScriptedStrategy strategy(
      [](const Bar&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          backtester.buy(5.0);
        } else if (index == 1) {
          backtester.close_position();
        }
      });

  engine.run(bars, strategy);

  CHECK_EQ(engine.fills().size(), 2u);
  CHECK_NEAR(engine.position(), 0.0, 1e-12);
  CHECK_NEAR(engine.realized_pnl(), 25.0, 1e-12);
}

TEST(pending_orders_at_the_end_are_not_executed) {
  const std::vector<Bar> bars = {make_bar(0, 100.0, 100.0)};

  BacktestConfig config;
  config.initial_cash = 1'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  Backtester engine(config);
  ScriptedStrategy strategy([](const Bar&, Backtester& backtester,
                               std::size_t) { backtester.buy(10.0); });

  const Performance performance = engine.run(bars, strategy);

  CHECK(engine.fills().empty());
  CHECK(engine.has_pending_orders());
  CHECK_EQ(engine.pending_order_count(), 1u);
  CHECK_NEAR(engine.position(), 0.0, 1e-12);
  CHECK_NEAR(performance.final_equity, 1'000.0, 1e-12);
}

TEST(performance_reports_return_drawdown_and_volatility) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_bar(86'400, 100.0, 120.0),
      make_bar(172'800, 120.0, 90.0),
      make_bar(259'200, 90.0, 110.0),
  };

  BacktestConfig config;
  config.initial_cash = 1'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  config.periods_per_year = 252.0;
  Backtester engine(config);

  BuyAndHoldStrategy strategy(10.0);
  const Performance performance = engine.run(bars, strategy);

  CHECK_NEAR(performance.final_equity, 1'100.0, 1e-12);
  CHECK_NEAR(performance.total_return, 0.10, 1e-12);
  CHECK_NEAR(performance.max_drawdown, 0.25, 1e-12);
  CHECK_NEAR(performance.turnover, 1'000.0 / 1'050.0, 1e-12);

  const auto& curve = engine.equity_curve();
  CHECK_EQ(curve.size(), 4u);

  std::vector<double> returns;
  for (std::size_t index = 1; index < curve.size(); ++index) {
    returns.push_back(curve[index] / curve[index - 1] - 1.0);
  }
  const double mean = (returns[0] + returns[1] + returns[2]) / 3.0;
  double variance = 0.0;
  for (const double value : returns) {
    variance += (value - mean) * (value - mean);
  }
  variance /= 2.0;
  const double deviation = std::sqrt(variance);

  CHECK_NEAR(performance.annualized_volatility, deviation * std::sqrt(252.0),
             1e-12);
  CHECK_NEAR(performance.sharpe_ratio, mean / deviation * std::sqrt(252.0),
             1e-12);
  CHECK(performance.annualized_return > 0.0);
}

TEST(sma_crossover_trades_a_trend) {
  std::vector<double> closes;
  for (int index = 0; index < 15; ++index) {
    closes.push_back(100.0);
  }
  for (int index = 0; index < 15; ++index) {
    closes.push_back(100.0 + 1.5 * static_cast<double>(index));
  }
  for (int index = 0; index < 15; ++index) {
    closes.push_back(122.5 - 1.5 * static_cast<double>(index));
  }

  const auto bars = bars_from_closes(closes);
  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  Backtester engine(config);

  SmaCrossoverStrategy strategy(3, 8, 10.0);
  const Performance performance = engine.run(bars, strategy);

  CHECK(engine.fills().size() >= 2u);
  CHECK_NEAR(engine.position(), 0.0, 1e-12);
  CHECK(engine.realized_pnl() > 0.0);
  CHECK(performance.final_equity > performance.initial_equity);
  CHECK(performance.winning_trades >= 1u);
}

TEST(sma_crossover_can_run_short) {
  std::vector<double> closes;
  for (int index = 0; index < 40; ++index) {
    closes.push_back(140.0 - 1.0 * static_cast<double>(index));
  }

  const auto bars = bars_from_closes(closes);
  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  Backtester engine(config);

  SmaCrossoverStrategy strategy(3, 8, 10.0, true);
  const Performance performance = engine.run(bars, strategy);

  CHECK(performance.final_position < 0.0);
  CHECK(performance.final_equity > performance.initial_equity);
}

TEST(csv_loader_reads_valid_files) {
  const auto path = temporary_path("cppquant_backtest_valid.csv");
  {
    std::ofstream file(path);
    file << "timestamp,open,high,low,close,volume\n";
    file << "0,100.0,101.0,99.0,100.5,1000\n";
    file << "86400,100.5,102.0,100.0,101.0,900\n";
  }

  const auto bars = load_bars_csv(path.string());
  std::remove(path.string().c_str());

  CHECK_EQ(bars.size(), 2u);
  CHECK_EQ(bars[0].timestamp, 0);
  CHECK_NEAR(bars[0].close, 100.5, 1e-12);
  CHECK_EQ(bars[1].timestamp, 86'400);
  CHECK_NEAR(bars[1].volume, 900.0, 1e-12);
}

TEST(csv_loader_accepts_files_without_a_header) {
  const auto path = temporary_path("cppquant_backtest_no_header.csv");
  {
    std::ofstream file(path);
    file << "0,100.0,101.0,99.0,100.5,1000\n";
  }

  const auto bars = load_bars_csv(path.string());
  std::remove(path.string().c_str());

  CHECK_EQ(bars.size(), 1u);
  CHECK_NEAR(bars[0].open, 100.0, 1e-12);
}

TEST(csv_loader_rejects_malformed_rows) {
  const auto bad_path = temporary_path("cppquant_backtest_bad.csv");
  {
    std::ofstream file(bad_path);
    file << "timestamp,open,high,low,close,volume\n";
    file << "0,100.0,99.0,101.0,100.0,1000\n";
  }
  CHECK_THROW(load_bars_csv(bad_path.string()), std::invalid_argument);
  std::remove(bad_path.string().c_str());

  CHECK_THROW(load_bars_csv("/definitely/not/a/file.csv"), std::runtime_error);
}

TEST(limit_buy_fills_at_the_open_on_a_gap) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_ohlc(86'400, 90.0, 92.0, 88.0, 91.0),
  };

  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  Backtester engine(config);

  OrderId id = 0;
  ScriptedStrategy strategy(
      [&](const Bar&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          id = backtester.buy_limit(10.0, 95.0);
        }
      });

  engine.run(bars, strategy);

  const OrderState* order = engine.find_order(id);
  CHECK(order != nullptr);
  CHECK(order->status == OrderStatus::Filled);
  CHECK_NEAR(order->filled_quantity, 10.0, 1e-12);
  CHECK_NEAR(order->average_fill_price, 90.0, 1e-12);
  CHECK_EQ(engine.fills().size(), 1u);
  CHECK_NEAR(engine.fills()[0].price, 90.0, 1e-12);
}

TEST(limit_buy_fills_at_the_limit_when_touched) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_ohlc(86'400, 100.0, 101.0, 94.0, 99.0),
  };

  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  Backtester engine(config);

  OrderId id = 0;
  ScriptedStrategy strategy(
      [&](const Bar&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          id = backtester.buy_limit(10.0, 95.0);
        }
      });

  engine.run(bars, strategy);

  CHECK_NEAR(engine.fills()[0].price, 95.0, 1e-12);
  CHECK_NEAR(engine.find_order(id)->average_fill_price, 95.0, 1e-12);
}

TEST(limit_buy_waits_until_the_limit_is_touched) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_ohlc(86'400, 99.0, 100.0, 96.0, 98.0),
      make_ohlc(172'800, 99.0, 100.0, 94.0, 97.0),
  };

  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  Backtester engine(config);

  OrderId id = 0;
  ScriptedStrategy strategy(
      [&](const Bar&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          id = backtester.buy_limit(10.0, 95.0);
        }
      });

  engine.run(bars, strategy);

  CHECK_EQ(engine.fills().size(), 1u);
  CHECK_EQ(engine.fills()[0].timestamp, 172'800);
  CHECK_NEAR(engine.fills()[0].price, 95.0, 1e-12);
  CHECK(engine.find_order(id)->status == OrderStatus::Filled);
}

TEST(sell_limit_fills_at_the_better_open_or_the_limit) {
  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;

  {
    const std::vector<Bar> bars = {
        make_bar(0, 100.0, 100.0),
        make_ohlc(86'400, 110.0, 111.0, 108.0, 109.0),
    };
    Backtester engine(config);
    ScriptedStrategy strategy(
        [](const Bar&, Backtester& backtester, std::size_t index) {
          if (index == 0) {
            backtester.sell_limit(5.0, 105.0);
          }
        });
    engine.run(bars, strategy);
    CHECK_NEAR(engine.fills()[0].price, 110.0, 1e-12);
  }

  {
    const std::vector<Bar> bars = {
        make_bar(0, 100.0, 100.0),
        make_ohlc(86'400, 100.0, 106.0, 99.0, 105.0),
    };
    Backtester engine(config);
    ScriptedStrategy strategy(
        [](const Bar&, Backtester& backtester, std::size_t index) {
          if (index == 0) {
            backtester.sell_limit(5.0, 105.0);
          }
        });
    engine.run(bars, strategy);
    CHECK_NEAR(engine.fills()[0].price, 105.0, 1e-12);
  }
}

TEST(stop_buy_triggers_at_the_open_on_a_gap) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_ohlc(86'400, 108.0, 110.0, 107.0, 109.0),
  };

  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  Backtester engine(config);

  OrderId id = 0;
  ScriptedStrategy strategy(
      [&](const Bar&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          id = backtester.buy_stop(10.0, 105.0);
        }
      });

  engine.run(bars, strategy);

  CHECK_NEAR(engine.fills()[0].price, 108.0, 1e-12);
  CHECK(engine.find_order(id)->status == OrderStatus::Filled);
}

TEST(stop_buy_triggers_intrabar_at_the_stop) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_ohlc(86'400, 100.0, 106.0, 99.0, 105.0),
  };

  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  Backtester engine(config);

  ScriptedStrategy strategy(
      [](const Bar&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          backtester.buy_stop(10.0, 105.0);
        }
      });

  engine.run(bars, strategy);

  CHECK_NEAR(engine.fills()[0].price, 105.0, 1e-12);
}

TEST(sell_stop_triggers_at_the_open_or_intrabar) {
  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;

  {
    const std::vector<Bar> bars = {
        make_bar(0, 100.0, 100.0),
        make_ohlc(86'400, 90.0, 92.0, 88.0, 91.0),
    };
    Backtester engine(config);
    ScriptedStrategy strategy(
        [](const Bar&, Backtester& backtester, std::size_t index) {
          if (index == 0) {
            backtester.sell_stop(5.0, 95.0);
          }
        });
    engine.run(bars, strategy);
    CHECK_NEAR(engine.fills()[0].price, 90.0, 1e-12);
  }

  {
    const std::vector<Bar> bars = {
        make_bar(0, 100.0, 100.0),
        make_ohlc(86'400, 100.0, 101.0, 94.0, 96.0),
    };
    Backtester engine(config);
    ScriptedStrategy strategy(
        [](const Bar&, Backtester& backtester, std::size_t index) {
          if (index == 0) {
            backtester.sell_stop(5.0, 95.0);
          }
        });
    engine.run(bars, strategy);
    CHECK_NEAR(engine.fills()[0].price, 95.0, 1e-12);
  }
}

TEST(stop_orders_pay_slippage_but_limits_do_not) {
  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 100.0; // 1%

  {
    const std::vector<Bar> bars = {
        make_bar(0, 100.0, 100.0),
        make_ohlc(86'400, 100.0, 106.0, 99.0, 105.0),
    };
    Backtester engine(config);
    ScriptedStrategy strategy(
        [](const Bar&, Backtester& backtester, std::size_t index) {
          if (index == 0) {
            backtester.buy_stop(5.0, 105.0);
          }
        });
    engine.run(bars, strategy);
    CHECK_NEAR(engine.fills()[0].price, 105.0 * 1.01, 1e-12);
  }

  {
    const std::vector<Bar> bars = {
        make_bar(0, 100.0, 100.0),
        make_ohlc(86'400, 90.0, 92.0, 88.0, 91.0),
    };
    Backtester engine(config);
    ScriptedStrategy strategy(
        [](const Bar&, Backtester& backtester, std::size_t index) {
          if (index == 0) {
            backtester.buy_limit(5.0, 95.0);
          }
        });
    engine.run(bars, strategy);
    CHECK_NEAR(engine.fills()[0].price, 90.0, 1e-12);
  }
}

TEST(stop_limit_gap_through_stop_fills_at_the_open) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_ohlc(86'400, 108.0, 112.0, 107.0, 110.0),
  };

  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  Backtester engine(config);

  OrderId id = 0;
  ScriptedStrategy strategy(
      [&](const Bar&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          id = backtester.buy_stop_limit(10.0, 105.0, 110.0);
        }
      });

  engine.run(bars, strategy);

  CHECK_NEAR(engine.fills()[0].price, 108.0, 1e-12);
  CHECK(engine.find_order(id)->status == OrderStatus::Filled);
}

TEST(stop_limit_intrabar_trigger_waits_for_the_next_bar) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_ohlc(86'400, 100.0, 108.0, 99.0, 107.0),
      make_ohlc(172'800, 104.0, 105.0, 102.0, 103.0),
  };

  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  Backtester engine(config);

  OrderId id = 0;
  ScriptedStrategy strategy(
      [&](const Bar&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          id = backtester.buy_stop_limit(10.0, 105.0, 106.0);
        }
      });

  engine.run(bars, strategy);

  const OrderState* order = engine.find_order(id);
  CHECK(order != nullptr);
  CHECK(order->triggered);
  CHECK(order->status == OrderStatus::Filled);
  CHECK_NEAR(order->average_fill_price, 104.0, 1e-12);
  CHECK_EQ(engine.fills().size(), 1u);
}

TEST(stop_limit_gap_outside_the_limit_becomes_a_limit_order) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_ohlc(86'400, 108.0, 110.0, 107.0, 109.0),
      make_ohlc(172'800, 103.0, 104.0, 102.0, 103.5),
  };

  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  Backtester engine(config);

  ScriptedStrategy strategy(
      [](const Bar&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          backtester.buy_stop_limit(10.0, 105.0, 104.0);
        }
      });

  engine.run(bars, strategy);

  CHECK_EQ(engine.fills().size(), 1u);
  CHECK_NEAR(engine.fills()[0].price, 103.0, 1e-12);
}

TEST(day_orders_expire_after_their_valid_bar) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_ohlc(86'400, 99.0, 100.0, 96.0, 98.0),
  };

  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  Backtester engine(config);

  OrderId id = 0;
  ScriptedStrategy strategy(
      [&](const Bar&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          id = backtester.buy_limit(10.0, 95.0, TimeInForce::Day);
        }
      });

  engine.run(bars, strategy);

  CHECK(engine.fills().empty());
  CHECK_EQ(engine.pending_order_count(), 0u);
  CHECK(engine.find_order(id)->status == OrderStatus::Expired);
}

TEST(day_orders_fill_on_their_valid_bar) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_ohlc(86'400, 99.0, 100.0, 94.0, 96.0),
  };

  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  Backtester engine(config);

  OrderId id = 0;
  ScriptedStrategy strategy(
      [&](const Bar&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          id = backtester.buy_limit(10.0, 95.0, TimeInForce::Day);
        }
      });

  engine.run(bars, strategy);

  CHECK_EQ(engine.fills().size(), 1u);
  CHECK(engine.find_order(id)->status == OrderStatus::Filled);
}

TEST(gtc_orders_survive_until_touched_or_cancelled) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_ohlc(86'400, 99.0, 100.0, 96.0, 98.0),
      make_ohlc(172'800, 98.0, 99.0, 94.0, 96.0),
  };

  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  Backtester engine(config);

  OrderId id = 0;
  ScriptedStrategy strategy(
      [&](const Bar&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          id = backtester.buy_limit(10.0, 95.0);
        }
      });

  engine.run(bars, strategy);

  CHECK_EQ(engine.fills().size(), 1u);
  CHECK_EQ(engine.fills()[0].timestamp, 172'800);
  CHECK(engine.find_order(id)->status == OrderStatus::Filled);
}

TEST(cancelling_an_order_prevents_execution) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_ohlc(86'400, 90.0, 92.0, 88.0, 91.0),
  };

  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  Backtester engine(config);

  OrderId id = 0;
  ScriptedStrategy strategy(
      [&](const Bar&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          id = backtester.buy_limit(10.0, 95.0);
          CHECK(backtester.cancel_order(id));
          CHECK(!backtester.cancel_order(id));
        }
      });

  engine.run(bars, strategy);

  CHECK(engine.fills().empty());
  CHECK_EQ(engine.pending_order_count(), 0u);
  CHECK(engine.find_order(id)->status == OrderStatus::Cancelled);
}

TEST(order_state_and_ids_are_queryable) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_ohlc(86'400, 100.0, 101.0, 99.0, 100.0),
  };

  Backtester engine;
  OrderId first = 0;
  OrderId second = 0;
  ScriptedStrategy strategy(
      [&](const Bar&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          first = backtester.buy_limit(10.0, 95.0);
          second = backtester.buy_stop(5.0, 105.0);
        }
      });

  engine.run(bars, strategy);

  CHECK(first != 0);
  CHECK(second > first);
  CHECK(engine.find_order(first) != nullptr);
  CHECK(engine.find_order(second) != nullptr);
  CHECK_EQ(engine.find_order(first)->type, OrderType::Limit);
  CHECK_EQ(engine.find_order(second)->type, OrderType::Stop);
  CHECK_EQ(engine.open_orders().size(), 2u);
}

TEST(order_target_accounts_for_pending_limit_orders) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_ohlc(86'400, 100.0, 101.0, 99.0, 100.0),
  };

  Backtester engine;
  ScriptedStrategy strategy(
      [](const Bar&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          backtester.buy_limit(10.0, 90.0);
          backtester.order_target(10.0);
        }
      });

  engine.run(bars, strategy);

  CHECK_EQ(engine.pending_order_count(), 1u);
  CHECK_EQ(engine.fills().size(), 0u);
  CHECK_NEAR(engine.position(), 0.0, 1e-12);
}

TEST(invalid_order_parameters_are_rejected) {
  Backtester engine;

  CHECK_THROW(engine.buy_limit(1.0, 0.0), std::invalid_argument);
  CHECK_THROW(engine.buy_limit(1.0, -5.0), std::invalid_argument);
  CHECK_THROW(engine.sell_limit(1.0, 0.0), std::invalid_argument);
  CHECK_THROW(engine.buy_stop(1.0, 0.0), std::invalid_argument);
  CHECK_THROW(engine.sell_stop(1.0, -5.0), std::invalid_argument);
  CHECK_THROW(engine.buy_stop_limit(1.0, 0.0, 100.0), std::invalid_argument);
  CHECK_THROW(engine.buy_stop_limit(1.0, 100.0, 0.0), std::invalid_argument);
  CHECK_THROW(engine.buy_limit(0.0, 100.0), std::invalid_argument);
  CHECK_THROW(engine.buy_limit(std::nan(""), 100.0), std::invalid_argument);
}

TEST(partial_fills_respect_volume_participation) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_ohlc(86'400, 100.0, 101.0, 99.0, 100.0, 100.0),
      make_ohlc(172'800, 100.0, 101.0, 99.0, 100.0, 100.0),
      make_ohlc(259'200, 100.0, 101.0, 99.0, 100.0, 100.0),
  };

  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  config.max_volume_participation = 0.1;
  Backtester engine(config);

  OrderId id = 0;
  ScriptedStrategy strategy(
      [&](const Bar&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          id = backtester.buy(25.0);
        }
      });

  engine.run(bars, strategy);

  CHECK_EQ(engine.fills().size(), 3u);
  CHECK_NEAR(engine.fills()[0].quantity, 10.0, 1e-12);
  CHECK_NEAR(engine.fills()[1].quantity, 10.0, 1e-12);
  CHECK_NEAR(engine.fills()[2].quantity, 5.0, 1e-12);
  CHECK(engine.find_order(id)->status == OrderStatus::Filled);
  CHECK_NEAR(engine.find_order(id)->filled_quantity, 25.0, 1e-12);
  CHECK_NEAR(engine.position(), 25.0, 1e-12);
}

TEST(zero_volume_bars_do_not_fill) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_ohlc(86'400, 100.0, 101.0, 99.0, 100.0, 0.0),
      make_ohlc(172'800, 100.0, 101.0, 99.0, 100.0, 100.0),
  };

  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  config.max_volume_participation = 0.1;
  Backtester engine(config);

  ScriptedStrategy strategy(
      [](const Bar&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          backtester.buy(10.0);
        }
      });

  engine.run(bars, strategy);

  CHECK_EQ(engine.fills().size(), 1u);
  CHECK_EQ(engine.fills()[0].timestamp, 172'800);
  CHECK_NEAR(engine.fills()[0].quantity, 10.0, 1e-12);
}

TEST(bar_liquidity_is_shared_across_orders) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_ohlc(86'400, 100.0, 101.0, 99.0, 100.0, 100.0),
      make_ohlc(172'800, 100.0, 101.0, 99.0, 100.0, 100.0),
  };

  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  config.max_volume_participation = 0.1;
  Backtester engine(config);

  OrderId first = 0;
  OrderId second = 0;
  ScriptedStrategy strategy(
      [&](const Bar&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          first = backtester.buy(10.0);
          second = backtester.buy(10.0);
        }
        if (index == 1) {
          CHECK(backtester.find_order(first)->status == OrderStatus::Filled);
          CHECK(backtester.find_order(second)->status == OrderStatus::Pending);
        }
      });

  engine.run(bars, strategy);

  CHECK_EQ(engine.fills().size(), 2u);
  CHECK(engine.find_order(first)->status == OrderStatus::Filled);
  CHECK(engine.find_order(second)->status == OrderStatus::Filled);
  CHECK_NEAR(engine.position(), 20.0, 1e-12);
}

TEST(day_partial_fills_expire_the_remainder) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_ohlc(86'400, 100.0, 101.0, 99.0, 100.0, 100.0),
  };

  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  config.max_volume_participation = 0.1;
  Backtester engine(config);

  OrderId id = 0;
  ScriptedStrategy strategy(
      [&](const Bar&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          id = backtester.buy(20.0, TimeInForce::Day);
        }
      });

  engine.run(bars, strategy);

  CHECK_EQ(engine.fills().size(), 1u);
  CHECK_NEAR(engine.fills()[0].quantity, 10.0, 1e-12);
  CHECK(engine.find_order(id)->status == OrderStatus::Expired);
  CHECK_NEAR(engine.find_order(id)->filled_quantity, 10.0, 1e-12);
  CHECK_EQ(engine.pending_order_count(), 0u);
}

TEST(gtc_partial_fills_continue_and_average) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_ohlc(86'400, 100.0, 101.0, 99.0, 100.0, 100.0),
      make_ohlc(172'800, 110.0, 111.0, 109.0, 110.0, 100.0),
  };

  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  config.max_volume_participation = 0.1;
  Backtester engine(config);

  OrderId id = 0;
  ScriptedStrategy strategy(
      [&](const Bar&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          id = backtester.buy(20.0);
        }
      });

  engine.run(bars, strategy);

  CHECK_EQ(engine.fills().size(), 2u);
  CHECK_NEAR(engine.fills()[0].price, 100.0, 1e-12);
  CHECK_NEAR(engine.fills()[1].price, 110.0, 1e-12);
  CHECK(engine.find_order(id)->status == OrderStatus::Filled);
  CHECK_NEAR(engine.find_order(id)->average_fill_price, 105.0, 1e-12);
  CHECK_NEAR(engine.cash(), 7'900.0, 1e-12);
}

TEST(cancelling_a_partially_filled_order_stops_the_remainder) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_ohlc(86'400, 100.0, 101.0, 99.0, 100.0, 100.0),
      make_ohlc(172'800, 100.0, 101.0, 99.0, 100.0, 100.0),
  };

  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  config.max_volume_participation = 0.1;
  Backtester engine(config);

  OrderId id = 0;
  ScriptedStrategy strategy(
      [&](const Bar&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          id = backtester.buy(20.0);
        } else if (index == 1) {
          CHECK(backtester.cancel_order(id));
        }
      });

  engine.run(bars, strategy);

  CHECK_EQ(engine.fills().size(), 1u);
  CHECK_NEAR(engine.fills()[0].quantity, 10.0, 1e-12);
  CHECK(engine.find_order(id)->status == OrderStatus::Cancelled);
  CHECK_NEAR(engine.position(), 10.0, 1e-12);
  CHECK_EQ(engine.pending_order_count(), 0u);
}

TEST(slippage_model_fixed_spread_and_impact) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_ohlc(86'400, 100.0, 101.0, 99.0, 100.0, 1'000.0),
  };

  {
    BacktestConfig config;
    config.initial_cash = 10'000.0;
    config.commission_bps = 0.0;
    config.slippage_model = SlippageModel::FixedBps;
    config.slippage_bps = 100.0;
    Backtester engine(config);
    ScriptedStrategy strategy(
        [](const Bar&, Backtester& backtester, std::size_t index) {
          if (index == 0) {
            backtester.buy(10.0);
          }
        });
    engine.run(bars, strategy);
    CHECK_NEAR(engine.fills()[0].price, 101.0, 1e-12);
  }

  {
    BacktestConfig config;
    config.initial_cash = 10'000.0;
    config.commission_bps = 0.0;
    config.slippage_model = SlippageModel::Spread;
    config.spread_bps = 200.0;
    config.slippage_bps = 999.0;
    Backtester engine(config);
    ScriptedStrategy strategy(
        [](const Bar&, Backtester& backtester, std::size_t index) {
          if (index == 0) {
            backtester.buy(10.0);
          }
        });
    engine.run(bars, strategy);
    CHECK_NEAR(engine.fills()[0].price, 101.0, 1e-12);
  }

  {
    BacktestConfig config;
    config.initial_cash = 10'000.0;
    config.commission_bps = 0.0;
    config.slippage_model = SlippageModel::Impact;
    config.slippage_bps = 0.0;
    config.impact_bps = 100.0;
    config.max_volume_participation = 1.0;
    Backtester engine(config);
    const std::vector<Bar> impact_bars = {
        make_bar(0, 100.0, 100.0),
        make_ohlc(86'400, 100.0, 101.0, 99.0, 100.0, 100.0),
    };
    ScriptedStrategy strategy(
        [](const Bar&, Backtester& backtester, std::size_t index) {
          if (index == 0) {
            backtester.buy(25.0);
          }
        });
    engine.run(impact_bars, strategy);
    CHECK_NEAR(engine.fills()[0].price, 100.5, 1e-12);
  }

  {
    BacktestConfig config;
    config.initial_cash = 10'000.0;
    config.commission_bps = 0.0;
    config.slippage_model = SlippageModel::Impact;
    config.impact_bps = 10'000.0;
    config.max_volume_participation = 1.0;
    Backtester engine(config);
    const std::vector<Bar> limit_bars = {
        make_bar(0, 100.0, 100.0),
        make_ohlc(86'400, 90.0, 92.0, 88.0, 91.0, 100.0),
    };
    ScriptedStrategy strategy(
        [](const Bar&, Backtester& backtester, std::size_t index) {
          if (index == 0) {
            backtester.buy_limit(10.0, 95.0);
          }
        });
    engine.run(limit_bars, strategy);
    CHECK_NEAR(engine.fills()[0].price, 90.0, 1e-12);
  }
}

TEST(partial_fill_commissions_are_charged_per_fill) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_ohlc(86'400, 100.0, 101.0, 99.0, 100.0, 100.0),
      make_ohlc(172'800, 110.0, 111.0, 109.0, 110.0, 100.0),
  };

  BacktestConfig config;
  config.initial_cash = 10'000.0;
  config.commission_bps = 1.0;
  config.slippage_bps = 0.0;
  config.max_volume_participation = 0.1;
  Backtester engine(config);

  ScriptedStrategy strategy(
      [](const Bar&, Backtester& backtester, std::size_t index) {
        if (index == 0) {
          backtester.buy(20.0);
        }
      });

  const Performance performance = engine.run(bars, strategy);

  CHECK_EQ(engine.fills().size(), 2u);
  CHECK_NEAR(engine.fills()[0].commission, 0.10, 1e-12);
  CHECK_NEAR(engine.fills()[1].commission, 0.11, 1e-12);
  CHECK_NEAR(performance.total_commission, 0.21, 1e-12);
  CHECK_NEAR(engine.cash(), 10'000.0 - 1'000.10 - 1'100.11, 1e-9);
}

TEST(volume_participation_and_slippage_settings_are_validated) {
  BacktestConfig config;
  config.max_volume_participation = 0.0;
  CHECK_THROW(Backtester{config}, std::invalid_argument);

  config.max_volume_participation = 1.5;
  CHECK_THROW(Backtester{config}, std::invalid_argument);

  config.max_volume_participation = 1.0;
  config.spread_bps = -1.0;
  CHECK_THROW(Backtester{config}, std::invalid_argument);

  config.spread_bps = 0.0;
  config.impact_bps = -1.0;
  CHECK_THROW(Backtester{config}, std::invalid_argument);

  config.impact_bps = 0.0;
  Backtester valid(config);
  (void)valid;
}

TEST(invalid_inputs_are_rejected) {
  BacktestConfig config;
  config.initial_cash = 0.0;
  CHECK_THROW(Backtester{config}, std::invalid_argument);

  Backtester engine;

  const std::vector<Bar> empty;
  ScriptedStrategy noop([](const Bar&, Backtester&, std::size_t) {});
  CHECK_THROW(engine.run(empty, noop), std::invalid_argument);

  const std::vector<Bar> bad_bar = {make_bar(0, -100.0, 100.0)};
  CHECK_THROW(engine.run(bad_bar, noop), std::invalid_argument);

  CHECK_THROW(engine.buy(0.0), std::invalid_argument);
  CHECK_THROW(engine.sell(-1.0), std::invalid_argument);
  CHECK_THROW(engine.buy(std::nan("")), std::invalid_argument);
  CHECK_THROW(engine.order_target(std::nan("")), std::invalid_argument);

  CHECK_THROW(SmaCrossoverStrategy(5, 5, 10.0), std::invalid_argument);
  CHECK_THROW(SmaCrossoverStrategy(0, 10, 10.0), std::invalid_argument);
  CHECK_THROW(SmaCrossoverStrategy(2, 10, 0.0), std::invalid_argument);
  CHECK_THROW(BuyAndHoldStrategy(0.0), std::invalid_argument);
}

TEST(run_resets_state_between_backtests) {
  const std::vector<Bar> bars = {
      make_bar(0, 100.0, 100.0),
      make_bar(86'400, 100.0, 110.0),
  };

  Backtester engine;
  BuyAndHoldStrategy first(10.0);
  engine.run(bars, first);

  BuyAndHoldStrategy second(5.0);
  const Performance performance = engine.run(bars, second);

  CHECK_EQ(engine.fills().size(), 1u);
  CHECK_NEAR(engine.position(), 5.0, 1e-12);
  CHECK_NEAR(performance.final_position, 5.0, 1e-12);
  CHECK_EQ(engine.equity_curve().size(), 2u);
}
