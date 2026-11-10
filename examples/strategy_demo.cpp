#include "cppquant/backtest.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace cppquant;

namespace {

class MomentumStrategy : public Strategy {
public:
  MomentumStrategy(int lookback, double quantity, double threshold)
      : lookback_(lookback), quantity_(quantity), threshold_(threshold) {}

  void on_bar(const Bar& bar, Backtester& engine) override {
    closes_.push_back(bar.close);
    if (static_cast<int>(closes_.size()) <= lookback_) {
      return;
    }
    const double past = closes_.front();
    if (bar.close > past * (1.0 + threshold_)) {
      engine.order_target(quantity_);
    } else if (bar.close < past * (1.0 - threshold_)) {
      engine.order_target(0.0);
    }
    closes_.pop_front();
  }

private:
  int lookback_{};
  double quantity_{};
  double threshold_{};
  std::deque<double> closes_;
};

class MeanReversionStrategy : public Strategy {
public:
  MeanReversionStrategy(int lookback, double quantity, double zscore)
      : lookback_(lookback), quantity_(quantity), zscore_(zscore) {}

  void on_bar(const Bar& bar, Backtester& engine) override {
    closes_.push_back(bar.close);
    sum_ += bar.close;
    sum_squares_ += bar.close * bar.close;
    if (static_cast<int>(closes_.size()) > lookback_) {
      const double removed = closes_.front();
      closes_.pop_front();
      sum_ -= removed;
      sum_squares_ -= removed * removed;
    }
    if (static_cast<int>(closes_.size()) < lookback_) {
      return;
    }

    const double count = static_cast<double>(closes_.size());
    const double mean = sum_ / count;
    const double variance = std::max(0.0, sum_squares_ / count - mean * mean);
    const double deviation = std::sqrt(variance);
    if (!(deviation > 1e-12)) {
      return;
    }
    if (bar.close < mean - zscore_ * deviation) {
      engine.order_target(quantity_);
    } else if (bar.close > mean + zscore_ * deviation) {
      engine.order_target(0.0);
    }
  }

private:
  int lookback_{};
  double quantity_{};
  double zscore_{};
  std::deque<double> closes_;
  double sum_{};
  double sum_squares_{};
};

class RebalanceStrategy : public BatchStrategy {
public:
  explicit RebalanceStrategy(std::size_t every) : every_(every) {}

  void on_batch(const BarBatch& batch, Backtester& engine) override {
    if (every_ == 0 || counter_++ % every_ != 0 || batch.bars.empty()) {
      return;
    }
    const double equity = engine.portfolio_equity();
    const double per_symbol = equity / static_cast<double>(batch.bars.size());
    for (const auto& entry : batch.bars) {
      if (entry.bar.close > 0.0) {
        engine.order_target(entry.symbol, per_symbol / entry.bar.close);
      }
    }
  }

private:
  std::size_t every_{};
  std::size_t counter_{};
};

struct Options {
  std::string bars_path;
  std::string panel_path;
  std::string strategy{"momentum"};
  int lookback{20};
  double quantity{100.0};
  double threshold{0.02};
  double zscore{1.0};
  std::size_t rebalance_every{5};
};

double next_number(int& index, int argc, char** argv, const char* flag) {
  if (index + 1 >= argc) {
    throw std::invalid_argument(std::string("missing value for ") + flag);
  }
  return std::stod(argv[++index]);
}

void print_usage(const char* program) {
  std::printf(
      "usage: %s [options]\n"
      "\n"
      "  --strategy momentum|mean-reversion|rebalance\n"
      "  --bars PATH              OHLCV CSV for single-symbol strategies\n"
      "  --panel PATH             long-format panel CSV for rebalance\n"
      "  --lookback N             momentum/mean-reversion window (default 20)\n"
      "  --quantity N             order quantity (default 100)\n"
      "  --threshold N            momentum return threshold (default 0.02)\n"
      "  --zscore N               mean-reversion z-score (default 1.0)\n"
      "  --rebalance-every N      batches between rebalances (default 5)\n"
      "  --help                   show this message\n",
      program);
}

Options parse(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string flag = argv[index];
    if (flag == "--help" || flag == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    } else if (flag == "--strategy") {
      if (index + 1 >= argc) {
        throw std::invalid_argument("missing value for --strategy");
      }
      options.strategy = argv[++index];
    } else if (flag == "--bars") {
      if (index + 1 >= argc) {
        throw std::invalid_argument("missing value for --bars");
      }
      options.bars_path = argv[++index];
    } else if (flag == "--panel") {
      if (index + 1 >= argc) {
        throw std::invalid_argument("missing value for --panel");
      }
      options.panel_path = argv[++index];
    } else if (flag == "--lookback") {
      options.lookback =
          static_cast<int>(next_number(index, argc, argv, "--lookback"));
    } else if (flag == "--quantity") {
      options.quantity = next_number(index, argc, argv, "--quantity");
    } else if (flag == "--threshold") {
      options.threshold = next_number(index, argc, argv, "--threshold");
    } else if (flag == "--zscore") {
      options.zscore = next_number(index, argc, argv, "--zscore");
    } else if (flag == "--rebalance-every") {
      options.rebalance_every = static_cast<std::size_t>(
          next_number(index, argc, argv, "--rebalance-every"));
    } else {
      throw std::invalid_argument("unknown argument: " + flag);
    }
  }
  if (options.strategy != "momentum" && options.strategy != "mean-reversion" &&
      options.strategy != "rebalance") {
    throw std::invalid_argument(
        "--strategy must be momentum, mean-reversion or rebalance");
  }
  if (options.lookback < 1) {
    throw std::invalid_argument("--lookback must be positive");
  }
  if (!(options.quantity > 0.0)) {
    throw std::invalid_argument("--quantity must be positive");
  }
  if (!(options.threshold >= 0.0)) {
    throw std::invalid_argument("--threshold must be non-negative");
  }
  if (!(options.zscore > 0.0)) {
    throw std::invalid_argument("--zscore must be positive");
  }
  if (options.rebalance_every == 0) {
    throw std::invalid_argument("--rebalance-every must be positive");
  }
  return options;
}

void print_performance(const Performance& performance) {
  std::printf("  initial equity      %14.2f\n", performance.initial_equity);
  std::printf("  final equity        %14.2f\n", performance.final_equity);
  std::printf("  total return        %13.2f%%\n",
              performance.total_return * 100.0);
  std::printf("  annualized return   %13.2f%%\n",
              performance.annualized_return * 100.0);
  std::printf("  annualized vol      %13.2f%%\n",
              performance.annualized_volatility * 100.0);
  std::printf("  sharpe ratio        %14.3f\n", performance.sharpe_ratio);
  std::printf("  max drawdown        %13.2f%%\n",
              performance.max_drawdown * 100.0);
  std::printf("  trades              %14zu\n", performance.trades);
  std::printf("  commission paid     %14.2f\n", performance.total_commission);
  std::printf("  realized pnl        %14.2f\n", performance.realized_pnl);
  std::printf("  final position      %14.4f\n", performance.final_position);
}

} // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse(argc, argv);
    BacktestConfig config;
    config.initial_cash = 100'000.0;
    config.commission_bps = 1.0;
    config.slippage_bps = 1.0;

    std::printf("cppquant-backtest strategy demo\n");
    if (options.strategy == "rebalance") {
      if (options.panel_path.empty()) {
        throw std::invalid_argument("--panel is required for rebalance");
      }
      const BarPanel panel = load_bar_panel_csv(options.panel_path);
      if (panel.batches().empty()) {
        throw std::runtime_error("panel has no batches");
      }
      Backtester engine(config);
      RebalanceStrategy strategy(options.rebalance_every);
      const Performance result =
          engine.run_batch(panel.batches(), panel.symbols().front(), strategy);

      std::printf("strategy: equal-weight rebalance every %zu batches\n",
                  options.rebalance_every);
      std::printf("file: %s, symbols: %zu, batches: %zu\n",
                  options.panel_path.c_str(), panel.symbols().size(),
                  panel.batches().size());
      print_performance(result);
      std::printf("  final positions:");
      if (result.final_positions.empty()) {
        std::printf(" (flat)");
      } else {
        for (const auto& [symbol, position] : result.final_positions) {
          std::printf(" %s=%.2f", symbol.c_str(), position);
        }
      }
      std::printf("\n");
      return 0;
    }

    if (options.bars_path.empty()) {
      throw std::invalid_argument("--bars is required for single-symbol demos");
    }
    const auto bars = load_bars_csv(options.bars_path);
    Backtester engine(config);
    std::printf("file: %s, bars: %zu\n", options.bars_path.c_str(),
                bars.size());
    if (options.strategy == "momentum") {
      MomentumStrategy strategy(options.lookback, options.quantity,
                                options.threshold);
      std::printf("strategy: momentum lookback=%d threshold=%.3f qty=%.2f\n",
                  options.lookback, options.threshold, options.quantity);
      print_performance(engine.run(bars, strategy));
    } else {
      MeanReversionStrategy strategy(options.lookback, options.quantity,
                                     options.zscore);
      std::printf("strategy: mean-reversion lookback=%d z=%.2f qty=%.2f\n",
                  options.lookback, options.zscore, options.quantity);
      print_performance(engine.run(bars, strategy));
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    return 1;
  }
  return 0;
}