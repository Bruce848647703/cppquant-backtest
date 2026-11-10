#include "cppquant/backtest.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>

using namespace cppquant;

namespace {

class CrossAssetSignal : public BatchStrategy {
public:
  CrossAssetSignal(std::string trade_symbol, std::string signal_symbol,
                   double quantity)
      : trade_symbol_(std::move(trade_symbol)),
        signal_symbol_(std::move(signal_symbol)), quantity_(quantity) {}

  void on_batch(const BarBatch& batch, Backtester& engine) override {
    const Bar* trade = batch.find(trade_symbol_);
    const Bar* signal = batch.find(signal_symbol_);
    if (trade == nullptr || signal == nullptr) {
      return;
    }
    if (signal->close > trade->close) {
      engine.order_target(quantity_);
    } else {
      engine.order_target(0.0);
    }
  }

private:
  std::string trade_symbol_;
  std::string signal_symbol_;
  double quantity_{};
};

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc > 3) {
      std::fprintf(stderr, "usage: %s [panel.csv] [quantity]\n", argv[0]);
      return 2;
    }

    const std::string path = argc > 1 ? argv[1] : "data/sample_multi.csv";
    const double quantity = argc > 2 ? std::stod(argv[2]) : 10.0;
    if (!(quantity > 0.0)) {
      throw std::invalid_argument("quantity must be positive");
    }

    const BarPanel panel = load_bar_panel_csv(path);
    if (panel.symbols().size() < 2) {
      throw std::invalid_argument("panel backtest needs at least two symbols");
    }

    const std::string trade_symbol = panel.symbols()[0];
    const std::string signal_symbol = panel.symbols()[1];

    BacktestConfig config;
    config.initial_cash = 100'000.0;
    config.max_volume_participation = 0.10;

    Backtester engine(config);
    CrossAssetSignal strategy(trade_symbol, signal_symbol, quantity);
    const Performance result =
        engine.run_batch(panel.batches(), trade_symbol, strategy);

    std::printf("cppquant-backtest panel\n");
    std::printf("file: %s\n", path.c_str());
    std::printf("symbols: %zu, batches: %zu, missing bars: %zu\n",
                panel.symbols().size(), panel.batches().size(),
                panel.missing_bars());
    std::printf("trade: %s, signal: %s\n", trade_symbol.c_str(),
                signal_symbol.c_str());
    std::printf("final equity: %.2f\n", result.final_equity);
    std::printf("total return: %.2f%%\n", result.total_return * 100.0);
    std::printf("trades: %zu, final position: %.4f\n", result.trades,
                result.final_position);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    return 1;
  }

  return 0;
}
