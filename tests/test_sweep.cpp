#include "cppquant/sweep.hpp"
#include "cppquant/walk_forward.hpp"
#include "test_framework.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

using namespace cppquant;

namespace {

Bar bar(std::size_t index, double price, double volume = 1'000'000.0) {
  return Bar{static_cast<std::int64_t>(index) * 86'400,
             price,
             price + 0.2,
             price - 0.2,
             price,
             volume};
}

std::vector<Bar> oscillating_bars(std::size_t count) {
  std::vector<Bar> bars;
  bars.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const double trend = 100.0 + 0.01 * static_cast<double>(index);
    const double cycle = 0.8 * std::sin(static_cast<double>(index) / 9.0);
    bars.push_back(bar(index, trend + cycle));
  }
  return bars;
}

void zero_cost(BacktestConfig& config) {
  config.initial_cash = 100'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  config.max_volume_participation = 1.0;
}

void check_same_performance(const Performance& left, const Performance& right) {
  CHECK_EQ(left.bars, right.bars);
  CHECK_EQ(left.trades, right.trades);
  CHECK_EQ(left.winning_trades, right.winning_trades);
  CHECK_EQ(left.losing_trades, right.losing_trades);
  CHECK_NEAR(left.initial_equity, right.initial_equity, 1e-12);
  CHECK_NEAR(left.final_equity, right.final_equity, 1e-12);
  CHECK_NEAR(left.total_return, right.total_return, 1e-12);
  CHECK_NEAR(left.sharpe_ratio, right.sharpe_ratio, 1e-12);
  CHECK_NEAR(left.max_drawdown, right.max_drawdown, 1e-12);
  CHECK_NEAR(left.turnover, right.turnover, 1e-12);
}

} // namespace

TEST(sma_sweep_matches_serial_and_parallel) {
  const auto bars = oscillating_bars(2'000);
  const auto grid = make_sma_grid({2, 5, 10}, {20, 30, 40});

  SmaSweepConfig config;
  zero_cost(config.backtest);
  config.quantity = 10.0;

  config.threads = 1;
  const auto serial = run_sma_sweep(bars, grid, config);
  config.threads = 4;
  const auto parallel = run_sma_sweep(bars, grid, config);

  CHECK_EQ(serial.size(), grid.size());
  CHECK_EQ(parallel.size(), grid.size());
  for (std::size_t index = 0; index < grid.size(); ++index) {
    CHECK_EQ(serial[index].parameters.fast_window, grid[index].fast_window);
    CHECK_EQ(serial[index].parameters.slow_window, grid[index].slow_window);
    CHECK_EQ(parallel[index].parameters.fast_window, grid[index].fast_window);
    check_same_performance(serial[index].performance,
                           parallel[index].performance);
  }
}

TEST(sma_sweep_matches_a_direct_backtest) {
  const auto bars = oscillating_bars(1'000);
  const std::vector<SmaParameters> grid{
      SmaParameters{2, 20}, SmaParameters{5, 30}, SmaParameters{10, 40}};

  SmaSweepConfig config;
  zero_cost(config.backtest);
  config.quantity = 10.0;
  config.threads = 2;
  const auto sweep = run_sma_sweep(bars, grid, config);

  for (std::size_t index = 0; index < grid.size(); ++index) {
    Backtester engine(config.backtest);
    SmaCrossoverStrategy strategy(grid[index].fast_window,
                                  grid[index].slow_window, config.quantity);
    const Performance direct = engine.run(bars, strategy);
    check_same_performance(sweep[index].performance, direct);
  }
}

TEST(sma_sweep_resolves_workers_and_rejects_invalid_inputs) {
  CHECK_EQ(resolve_thread_count(1), 1u);
  CHECK_EQ(resolve_thread_count(3), 3u);
  CHECK(resolve_thread_count(0) >= 1u);

  const auto bars = oscillating_bars(100);
  const auto grid = make_sma_grid({2}, {10});
  SmaSweepConfig config;
  zero_cost(config.backtest);
  config.quantity = 10.0;
  config.threads = 2;

  CHECK_THROW(run_sma_sweep({}, grid, config), std::invalid_argument);
  CHECK_THROW(run_sma_sweep(bars, {}, config), std::invalid_argument);
  CHECK_THROW(run_sma_sweep(bars, {SmaParameters{5, 5}}, config),
              std::invalid_argument);
  CHECK_THROW(run_sma_sweep(bars, {SmaParameters{0, 5}}, config),
              std::invalid_argument);

  config.quantity = 0.0;
  CHECK_THROW(run_sma_sweep(bars, grid, config), std::invalid_argument);

  config.quantity = 10.0;
  config.backtest.initial_cash = 0.0;
  CHECK_THROW(run_sma_sweep(bars, grid, config), std::invalid_argument);
}

TEST(walk_forward_parallel_matches_serial) {
  const auto bars = oscillating_bars(300);
  const auto grid = make_sma_grid({2, 5}, {20, 30});

  WalkForwardConfig config;
  zero_cost(config.backtest);
  config.train_bars = 80;
  config.test_bars = 40;
  config.quantity = 10.0;

  config.threads = 1;
  const auto serial = run_walk_forward(bars, grid, config);
  config.threads = 4;
  const auto parallel = run_walk_forward(bars, grid, config);

  CHECK_EQ(serial.windows.size(), parallel.windows.size());
  CHECK(!serial.windows.empty());
  for (std::size_t index = 0; index < serial.windows.size(); ++index) {
    const auto& left = serial.windows[index];
    const auto& right = parallel.windows[index];
    CHECK_EQ(left.train_start, right.train_start);
    CHECK_EQ(left.train_end, right.train_end);
    CHECK_EQ(left.test_start, right.test_start);
    CHECK_EQ(left.test_end, right.test_end);
    CHECK_EQ(left.parameters.fast_window, right.parameters.fast_window);
    CHECK_EQ(left.parameters.slow_window, right.parameters.slow_window);
    check_same_performance(left.training, right.training);
    check_same_performance(left.testing, right.testing);
  }

  CHECK_EQ(serial.equity_curve.size(), parallel.equity_curve.size());
  for (std::size_t index = 0; index < serial.equity_curve.size(); ++index) {
    CHECK_NEAR(serial.equity_curve[index], parallel.equity_curve[index], 1e-12);
  }
  CHECK_NEAR(serial.final_equity, parallel.final_equity, 1e-12);
  CHECK_NEAR(serial.sharpe_ratio, parallel.sharpe_ratio, 1e-12);
}