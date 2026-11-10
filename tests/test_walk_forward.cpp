#include "cppquant/walk_forward.hpp"
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

Bar bar(std::size_t index, double price, double volume = 1'000'000.0) {
  return Bar{static_cast<std::int64_t>(index) * 86'400,
             price,
             price + 0.5,
             price - 0.5,
             price,
             volume};
}

std::vector<Bar> trending_bars(std::size_t count, double start, double step) {
  std::vector<Bar> bars;
  bars.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    bars.push_back(bar(index, start + step * static_cast<double>(index)));
  }
  return bars;
}

void zero_cost(BacktestConfig& config) {
  config.initial_cash = 100'000.0;
  config.commission_bps = 0.0;
  config.slippage_bps = 0.0;
  config.max_volume_participation = 1.0;
}

std::filesystem::path temporary_path(const std::string& name) {
  return std::filesystem::temp_directory_path() / name;
}

} // namespace

TEST(make_sma_grid_rejects_invalid_windows) {
  const auto grid = make_sma_grid({2, 5}, {10, 20});
  CHECK_EQ(grid.size(), 4u);

  CHECK_THROW(make_sma_grid({}, {10}), std::invalid_argument);
  CHECK_THROW(make_sma_grid({5}, {5}), std::invalid_argument);
  CHECK_THROW(make_sma_grid({0}, {10}), std::invalid_argument);
}

TEST(walk_forward_windows_are_ordered_and_non_overlapping) {
  const auto bars = trending_bars(200, 100.0, 0.5);
  WalkForwardConfig config;
  zero_cost(config.backtest);
  config.train_bars = 60;
  config.test_bars = 20;
  config.quantity = 10.0;

  const auto result =
      run_walk_forward(bars, make_sma_grid({2, 5}, {10, 20}), config);

  CHECK_EQ(result.windows.size(), 7u);
  for (std::size_t index = 0; index < result.windows.size(); ++index) {
    const auto& window = result.windows[index];
    CHECK_EQ(window.train_end, window.test_start);
    CHECK(window.test_end > window.test_start);
    CHECK(window.test_end <= bars.size());
    if (index > 0) {
      CHECK(window.test_start >= result.windows[index - 1].test_end);
      CHECK(window.train_start > result.windows[index - 1].train_start);
    }
  }
  CHECK_EQ(result.equity_curve.size(), 7u * 20u);
  CHECK_EQ(result.timestamps.size(), result.equity_curve.size());
  for (std::size_t index = 1; index < result.timestamps.size(); ++index) {
    CHECK(result.timestamps[index] > result.timestamps[index - 1]);
  }
}

TEST(walk_forward_selects_the_best_training_parameter) {
  const auto bars = trending_bars(200, 100.0, 0.5);
  WalkForwardConfig config;
  zero_cost(config.backtest);
  config.train_bars = 60;
  config.test_bars = 20;
  config.quantity = 10.0;

  const auto result =
      run_walk_forward(bars, make_sma_grid({2, 10}, {5, 20}), config);

  CHECK(!result.windows.empty());
  for (const auto& window : result.windows) {
    CHECK_EQ(window.parameters.fast_window, 2);
    CHECK_EQ(window.parameters.slow_window, 5);
  }
}

TEST(walk_forward_compounds_oos_equity) {
  const auto bars = trending_bars(200, 100.0, 0.5);
  WalkForwardConfig config;
  zero_cost(config.backtest);
  config.train_bars = 60;
  config.test_bars = 20;
  config.quantity = 10.0;

  const auto result = run_walk_forward(bars, make_sma_grid({2}, {10}), config);

  CHECK_NEAR(result.initial_equity, 100'000.0, 1e-12);
  CHECK_NEAR(result.final_equity, result.equity_curve.back(), 1e-12);
  CHECK_NEAR(result.total_return,
             result.final_equity / result.initial_equity - 1.0, 1e-12);
  CHECK(result.final_equity > result.initial_equity);
  CHECK(result.annualized_volatility >= 0.0);
  CHECK(result.max_drawdown >= 0.0);
  CHECK_EQ(result.bars, result.equity_curve.size());
}

TEST(walk_forward_handles_a_short_final_test_window) {
  const auto bars = trending_bars(70, 100.0, 0.5);
  WalkForwardConfig config;
  zero_cost(config.backtest);
  config.train_bars = 40;
  config.test_bars = 20;
  config.quantity = 10.0;

  const auto result = run_walk_forward(bars, make_sma_grid({2}, {10}), config);

  CHECK_EQ(result.windows.size(), 2u);
  CHECK_EQ(result.windows[0].test_start, 40u);
  CHECK_EQ(result.windows[0].test_end, 60u);
  CHECK_EQ(result.windows[1].test_start, 60u);
  CHECK_EQ(result.windows[1].test_end, 70u);
  CHECK_EQ(result.equity_curve.size(), 30u);
}

TEST(walk_forward_returns_no_windows_without_enough_data) {
  const auto bars = trending_bars(30, 100.0, 0.5);
  WalkForwardConfig config;
  zero_cost(config.backtest);
  config.train_bars = 40;
  config.test_bars = 20;

  const auto result = run_walk_forward(bars, make_sma_grid({2}, {10}), config);

  CHECK(result.windows.empty());
  CHECK(result.equity_curve.empty());
  CHECK_NEAR(result.final_equity, 100'000.0, 1e-12);
  CHECK_NEAR(result.total_return, 0.0, 1e-12);
}

TEST(walk_forward_rejects_overlapping_test_windows) {
  const auto bars = trending_bars(100, 100.0, 0.5);
  WalkForwardConfig config;
  zero_cost(config.backtest);
  config.train_bars = 40;
  config.test_bars = 20;
  config.step_bars = 10;

  CHECK_THROW(run_walk_forward(bars, make_sma_grid({2}, {10}), config),
              std::invalid_argument);

  config.step_bars = 0;
  CHECK_THROW(run_walk_forward(bars, {}, config), std::invalid_argument);
}

TEST(walk_forward_is_reproducible) {
  const auto bars = trending_bars(120, 100.0, 0.4);
  WalkForwardConfig config;
  zero_cost(config.backtest);
  config.train_bars = 50;
  config.test_bars = 20;
  config.quantity = 10.0;
  const auto grid = make_sma_grid({2, 5}, {10, 20});

  const auto first = run_walk_forward(bars, grid, config);
  const auto second = run_walk_forward(bars, grid, config);

  CHECK_EQ(first.windows.size(), second.windows.size());
  CHECK(first.equity_curve == second.equity_curve);
  CHECK(first.timestamps == second.timestamps);
  for (std::size_t index = 0; index < first.windows.size(); ++index) {
    CHECK_EQ(first.windows[index].parameters.fast_window,
             second.windows[index].parameters.fast_window);
    CHECK_EQ(first.windows[index].parameters.slow_window,
             second.windows[index].parameters.slow_window);
  }
}

TEST(walk_forward_exports_a_csv) {
  const auto bars = trending_bars(120, 100.0, 0.4);
  WalkForwardConfig config;
  zero_cost(config.backtest);
  config.train_bars = 50;
  config.test_bars = 20;
  config.quantity = 10.0;

  const auto result =
      run_walk_forward(bars, make_sma_grid({2, 5}, {10}), config);
  const auto path = temporary_path("cppquant_walk_forward.csv");
  write_walk_forward_csv(result, path.string());

  std::ifstream file(path);
  CHECK(file.good());
  std::string line;
  std::size_t lines = 0;
  bool header_seen = false;
  while (std::getline(file, line)) {
    ++lines;
    if (lines == 1) {
      header_seen = line.find("train_start") != std::string::npos;
    }
  }
  std::remove(path.string().c_str());

  CHECK(header_seen);
  CHECK_EQ(lines, result.windows.size() + 1);
  CHECK_THROW(write_walk_forward_csv(result, "/definitely/not/a/file.csv"),
              std::runtime_error);
}
