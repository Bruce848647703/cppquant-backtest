#pragma once

#include "cppquant/backtest.hpp"

#include <cstddef>
#include <vector>

namespace cppquant {

struct SmaParameters {
  int fast_window{};
  int slow_window{};
};

/// Cartesian product of fast and slow windows, keeping only `fast < slow`.
std::vector<SmaParameters> make_sma_grid(const std::vector<int>& fast_windows,
                                         const std::vector<int>& slow_windows);

struct SmaSweepConfig {
  BacktestConfig backtest;
  double quantity{100.0};
  /// Worker count. Zero selects hardware concurrency; one runs serially.
  std::size_t threads{0};
};

struct SmaSweepResult {
  SmaParameters parameters{};
  Performance performance{};
};

/// Resolve a requested worker count. Zero becomes
/// `std::thread::hardware_concurrency()`, with a fallback of one.
std::size_t resolve_thread_count(std::size_t requested) noexcept;

/// Run every SMA parameter pair independently and return results in grid order.
///
/// Each worker owns its own `Backtester`; the bar vector is read-only. Results
/// are stored by grid index, so serial and parallel runs are bit-for-bit
/// identical and ties select the earlier grid entry.
std::vector<SmaSweepResult>
run_sma_sweep(const std::vector<Bar>& bars,
              const std::vector<SmaParameters>& grid,
              const SmaSweepConfig& config = {});

} // namespace cppquant