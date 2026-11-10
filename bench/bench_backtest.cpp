#include "cppquant/backtest.hpp"
#include "cppquant/sweep.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace cppquant;

namespace {

using Clock = std::chrono::steady_clock;

std::vector<Bar> generate_bars(std::size_t count, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::normal_distribution<double> noise(0.0, 1.0);

  std::vector<Bar> bars;
  bars.reserve(count);
  double log_price = std::log(100.0);

  for (std::size_t index = 0; index < count; ++index) {
    const double open = std::exp(log_price);
    const double cycle = 0.00005 * std::sin(static_cast<double>(index) / 97.0);
    log_price += 0.0000005 + cycle + 0.001 * noise(rng);
    const double close = std::exp(log_price);
    const double high =
        std::max(open, close) * (1.0 + 0.0005 * std::abs(noise(rng)));
    const double low =
        std::min(open, close) * (1.0 - 0.0005 * std::abs(noise(rng)));

    bars.push_back(Bar{static_cast<std::int64_t>(index) * 86'400, open, high,
                       low, close,
                       1'000'000.0 + static_cast<double>(rng() % 500'000)});
  }

  return bars;
}

struct SweepRun {
  double seconds{};
  std::vector<SmaSweepResult> results;
};

SweepRun run_sweep(const std::vector<Bar>& bars, std::size_t threads,
                   bool quick) {
  const std::vector<SmaParameters> grid =
      quick ? make_sma_grid({5, 10, 15, 20}, {30, 45, 60, 75})
            : make_sma_grid({5, 10, 15, 20, 25, 30}, {30, 45, 60, 75, 90, 120});

  SmaSweepConfig config;
  config.backtest.initial_cash = 1'000'000.0;
  config.backtest.commission_bps = 1.0;
  config.backtest.slippage_bps = 1.0;
  config.quantity = 100.0;
  config.threads = threads;

  const auto start = Clock::now();
  SweepRun run;
  run.results = run_sma_sweep(bars, grid, config);
  run.seconds = std::chrono::duration<double>(Clock::now() - start).count();

  double checksum = 0.0;
  for (const auto& result : run.results) {
    checksum += result.performance.final_equity +
                result.performance.total_return +
                static_cast<double>(result.performance.trades);
  }
  std::printf("sweep %zu pairs, %zu threads  %.3f s  %.1f pairs/s  "
              "checksum %.6f\n",
              grid.size(), threads, run.seconds,
              static_cast<double>(grid.size()) / run.seconds, checksum);
  return run;
}

} // namespace

int main(int argc, char** argv) {
  std::size_t bar_count = 1'000'000;

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--quick") {
      bar_count = 100'000;
    } else if (argument == "--help" || argument == "-h") {
      std::printf("usage: %s [--quick]\n", argv[0]);
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argument.c_str());
      return 2;
    }
  }

  std::printf("cppquant-backtest benchmark\n");
  std::printf("generating %zu bars...\n", bar_count);
  const auto bars = generate_bars(bar_count, 42);

  BacktestConfig config;
  config.initial_cash = 1'000'000.0;
  config.commission_bps = 1.0;
  config.slippage_bps = 1.0;

  Backtester engine(config);
  SmaCrossoverStrategy strategy(10, 30, 100.0);

  const auto start = Clock::now();
  const Performance performance = engine.run(bars, strategy);
  const double seconds =
      std::chrono::duration<double>(Clock::now() - start).count();

  std::printf("%zu bars in %.3f s  %.2f M bars/s\n", bar_count, seconds,
              static_cast<double>(bar_count) / seconds / 1e6);
  std::printf("fills: %zu  final equity: %.2f  total return: %.2f%%\n",
              performance.trades, performance.final_equity,
              performance.total_return * 100.0);

  const bool quick = bar_count == 100'000;
  const SweepRun serial = run_sweep(bars, 1, quick);
  const SweepRun parallel = run_sweep(bars, 4, quick);
  double difference = 0.0;
  for (std::size_t index = 0; index < serial.results.size(); ++index) {
    difference = std::max(
        difference, std::abs(serial.results[index].performance.final_equity -
                             parallel.results[index].performance.final_equity));
  }
  std::printf("serial/parallel max equity difference: %.3e  speedup: %.2fx\n",
              difference, serial.seconds / parallel.seconds);
  return 0;
}
