#pragma once

#include "cppquant/backtest.hpp"
#include "cppquant/sweep.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace cppquant {

struct WalkForwardConfig {
  std::size_t train_bars{252};
  std::size_t test_bars{63};
  /// Distance between consecutive test windows. Zero means `test_bars`.
  /// Must be at least `test_bars` to avoid overlapping out-of-sample data.
  std::size_t step_bars{0};
  double quantity{100.0};
  /// Base backtest settings. `initial_cash` is replaced by the running OOS
  /// equity for each window.
  BacktestConfig backtest;
  /// Select by Sharpe ratio instead of total return on the training window.
  bool select_by_sharpe{false};
  /// Worker count for each training sweep. Zero selects hardware concurrency;
  /// one runs serially. Results are deterministic either way.
  std::size_t threads{0};
};

struct WalkForwardWindow {
  std::size_t train_start{};
  std::size_t train_end{};
  std::size_t test_start{};
  std::size_t test_end{};
  SmaParameters parameters{};
  Performance training;
  Performance testing;
};

struct WalkForwardResult {
  std::vector<WalkForwardWindow> windows;
  std::vector<double> equity_curve;
  std::vector<std::int64_t> timestamps;
  double initial_equity{};
  double final_equity{};
  double total_return{};
  double annualized_return{};
  double annualized_volatility{};
  double sharpe_ratio{};
  double max_drawdown{};
  std::size_t trades{};
  std::size_t bars{};
};

/// Rolling walk-forward evaluation for the SMA crossover strategy.
///
/// Each window selects the best parameter pair on `[train_start, train_end)`
/// and applies it only to the following `[test_start, test_end)` bars. Test
/// windows are non-overlapping when `step_bars >= test_bars`. Cash compounds
/// across windows; positions do not carry over.
WalkForwardResult run_walk_forward(const std::vector<Bar>& bars,
                                   const std::vector<SmaParameters>& grid,
                                   const WalkForwardConfig& config = {});

/// Write one row per window plus a header. Throws std::runtime_error when the
/// file cannot be opened.
void write_walk_forward_csv(const WalkForwardResult& result,
                            const std::string& path);

} // namespace cppquant
