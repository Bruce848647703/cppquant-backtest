#include "cppquant/sweep.hpp"

#include "validate.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cppquant {
namespace {

void validate_bars(const std::vector<Bar>& bars) {
  if (bars.empty()) {
    throw std::invalid_argument("sweep requires at least one bar");
  }
  for (std::size_t index = 0; index < bars.size(); ++index) {
    detail::validate_bar(bars[index], "bar " + std::to_string(index));
  }
}

void validate_grid(const std::vector<SmaParameters>& grid) {
  if (grid.empty()) {
    throw std::invalid_argument("sweep requires a parameter grid");
  }
  for (const auto& parameters : grid) {
    if (parameters.fast_window < 1 || parameters.slow_window < 1 ||
        parameters.fast_window >= parameters.slow_window) {
      throw std::invalid_argument("sweep grid requires positive fast < slow");
    }
  }
}

} // namespace

std::vector<SmaParameters> make_sma_grid(const std::vector<int>& fast_windows,
                                         const std::vector<int>& slow_windows) {
  if (fast_windows.empty() || slow_windows.empty()) {
    throw std::invalid_argument("SMA parameter grid must not be empty");
  }
  std::vector<SmaParameters> grid;
  grid.reserve(fast_windows.size() * slow_windows.size());
  for (const int fast : fast_windows) {
    for (const int slow : slow_windows) {
      if (fast < 1 || slow < 1) {
        throw std::invalid_argument(
            "SMA grid requires positive window lengths");
      }
      if (fast < slow) {
        grid.push_back(SmaParameters{fast, slow});
      }
    }
  }
  if (grid.empty()) {
    throw std::invalid_argument(
        "SMA grid requires at least one valid fast < slow pair");
  }
  return grid;
}

std::size_t resolve_thread_count(std::size_t requested) noexcept {
  if (requested > 0) {
    return requested;
  }
  const unsigned int hardware = std::thread::hardware_concurrency();
  return hardware == 0 ? 1 : static_cast<std::size_t>(hardware);
}

std::vector<SmaSweepResult>
run_sma_sweep(const std::vector<Bar>& bars,
              const std::vector<SmaParameters>& grid,
              const SmaSweepConfig& config) {
  validate_bars(bars);
  validate_grid(grid);
  if (!std::isfinite(config.quantity) || config.quantity <= 0.0) {
    throw std::invalid_argument("sweep quantity must be positive and finite");
  }

  std::vector<SmaSweepResult> results(grid.size());
  std::atomic<std::size_t> next{0};
  std::atomic<bool> failed{false};
  std::exception_ptr error;
  std::mutex error_mutex;

  auto worker = [&]() {
    while (!failed.load(std::memory_order_relaxed)) {
      const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
      if (index >= grid.size()) {
        return;
      }
      try {
        Backtester engine(config.backtest);
        SmaCrossoverStrategy strategy(grid[index].fast_window,
                                      grid[index].slow_window, config.quantity);
        SmaSweepResult result;
        result.parameters = grid[index];
        result.performance = engine.run(bars, strategy);
        results[index] = std::move(result);
      } catch (...) {
        std::lock_guard<std::mutex> lock(error_mutex);
        if (!error) {
          error = std::current_exception();
        }
        failed.store(true, std::memory_order_relaxed);
        return;
      }
    }
  };

  const std::size_t workers =
      std::min(resolve_thread_count(config.threads), grid.size());
  if (workers <= 1) {
    worker();
  } else {
    std::vector<std::thread> threads;
    threads.reserve(workers - 1);
    for (std::size_t index = 1; index < workers; ++index) {
      threads.emplace_back(worker);
    }
    worker();
    for (auto& thread : threads) {
      thread.join();
    }
  }

  if (error) {
    std::rethrow_exception(error);
  }
  return results;
}

} // namespace cppquant