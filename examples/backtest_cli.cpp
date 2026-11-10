#include "cppquant/backtest.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace cppquant;

namespace {

struct Config {
  std::string csv_path;
  std::string actions_path;
  std::string strategy{"sma"};
  int fast{10};
  int slow{30};
  double quantity{100.0};
  bool allow_short{false};
  bool print_fills{false};
  BacktestConfig backtest;
};

double next_value(int& index, int argc, char** argv, const char* flag) {
  if (index + 1 >= argc) {
    throw std::invalid_argument(std::string("missing value for ") + flag);
  }
  return std::stod(argv[++index]);
}

void print_usage(const char* program) {
  std::printf(
      "usage: %s --csv PATH [options]\n"
      "\n"
      "  --csv PATH               OHLCV CSV file (required)\n"
      "  --strategy sma|buyhold   strategy (default sma)\n"
      "  --fast N                 fast SMA window (default 10)\n"
      "  --slow N                 slow SMA window (default 30)\n"
      "  --quantity N             order quantity (default 100)\n"
      "  --short                  allow short positions in the SMA strategy\n"
      "  --cash N                 initial cash (default 100000)\n"
      "  --commission-bps N       commission in basis points (default 1)\n"
      "  --slippage-bps N         slippage in basis points (default 1)\n"
      "  --rf N                   annual risk-free rate (default 0)\n"
      "  --margin-rate N          annual rate on negative cash (default 0)\n"
      "  --borrow-rate N          annual rate on short market value (default "
      "0)\n"
      "  --periods-per-year N     bars per year (default 252)\n"
      "  --actions PATH           corporate actions CSV\n"
      "  --fills                  print the first fills\n"
      "  --help                   show this message\n",
      program);
}

Config parse(int argc, char** argv) {
  Config config;

  for (int index = 1; index < argc; ++index) {
    const std::string flag = argv[index];

    if (flag == "--help" || flag == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    } else if (flag == "--csv") {
      if (index + 1 >= argc) {
        throw std::invalid_argument("missing value for --csv");
      }
      config.csv_path = argv[++index];
    } else if (flag == "--actions") {
      if (index + 1 >= argc) {
        throw std::invalid_argument("missing value for --actions");
      }
      config.actions_path = argv[++index];
    } else if (flag == "--strategy") {
      if (index + 1 >= argc) {
        throw std::invalid_argument("missing value for --strategy");
      }
      config.strategy = argv[++index];
      if (config.strategy != "sma" && config.strategy != "buyhold") {
        throw std::invalid_argument("--strategy must be sma or buyhold");
      }
    } else if (flag == "--fast") {
      config.fast = static_cast<int>(next_value(index, argc, argv, "--fast"));
    } else if (flag == "--slow") {
      config.slow = static_cast<int>(next_value(index, argc, argv, "--slow"));
    } else if (flag == "--quantity") {
      config.quantity = next_value(index, argc, argv, "--quantity");
    } else if (flag == "--short") {
      config.allow_short = true;
    } else if (flag == "--cash") {
      config.backtest.initial_cash = next_value(index, argc, argv, "--cash");
    } else if (flag == "--commission-bps") {
      config.backtest.commission_bps =
          next_value(index, argc, argv, "--commission-bps");
    } else if (flag == "--slippage-bps") {
      config.backtest.slippage_bps =
          next_value(index, argc, argv, "--slippage-bps");
    } else if (flag == "--rf") {
      config.backtest.risk_free_rate = next_value(index, argc, argv, "--rf");
    } else if (flag == "--margin-rate") {
      config.backtest.margin_rate =
          next_value(index, argc, argv, "--margin-rate");
    } else if (flag == "--borrow-rate") {
      config.backtest.borrow_rate =
          next_value(index, argc, argv, "--borrow-rate");
    } else if (flag == "--periods-per-year") {
      config.backtest.periods_per_year =
          next_value(index, argc, argv, "--periods-per-year");
    } else if (flag == "--fills") {
      config.print_fills = true;
    } else {
      throw std::invalid_argument("unknown argument: " + flag);
    }
  }

  if (config.csv_path.empty()) {
    throw std::invalid_argument("--csv is required");
  }
  return config;
}

std::string format_timestamp(std::int64_t timestamp) {
  const std::time_t time = static_cast<std::time_t>(timestamp);
  const std::tm* broken = std::gmtime(&time);
  if (broken == nullptr) {
    return std::to_string(timestamp);
  }
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", broken);
  return buffer;
}

} // namespace

int main(int argc, char** argv) {
  try {
    const Config config = parse(argc, argv);
    const auto bars = load_bars_csv(config.csv_path);
    const auto actions = config.actions_path.empty()
                             ? std::vector<CorporateAction>{}
                             : load_corporate_actions_csv(config.actions_path);
    Backtester engine(config.backtest);

    std::unique_ptr<Strategy> strategy;
    if (config.strategy == "sma") {
      strategy = std::make_unique<SmaCrossoverStrategy>(
          config.fast, config.slow, config.quantity, config.allow_short);
    } else {
      strategy = std::make_unique<BuyAndHoldStrategy>(config.quantity);
    }

    const Performance performance = engine.run(bars, *strategy, actions);

    std::printf("cppquant-backtest\n");
    std::printf("data: %s\n", config.csv_path.c_str());
    std::printf("bars: %zu  (%s .. %s)\n", bars.size(),
                format_timestamp(bars.front().timestamp).c_str(),
                format_timestamp(bars.back().timestamp).c_str());
    if (config.strategy == "sma") {
      std::printf("strategy: sma fast=%d slow=%d qty=%.4f short=%s\n",
                  config.fast, config.slow, config.quantity,
                  config.allow_short ? "yes" : "no");
    } else {
      std::printf("strategy: buyhold qty=%.4f\n", config.quantity);
    }
    std::printf("\nperformance\n");
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
    std::printf("  turnover            %14.3f\n", performance.turnover);
    std::printf("  trades              %14zu\n", performance.trades);
    std::printf("  win rate            %13.2f%%\n",
                performance.win_rate * 100.0);
    std::printf("  commission paid     %14.2f\n", performance.total_commission);
    std::printf("  dividends           %14.2f\n", performance.total_dividends);
    std::printf("  margin cost         %14.2f\n",
                performance.total_margin_cost);
    std::printf("  borrow cost         %14.2f\n",
                performance.total_borrow_cost);
    std::printf("  realized pnl        %14.2f\n", performance.realized_pnl);
    std::printf("  final position      %14.4f\n\n", performance.final_position);

    if (config.print_fills) {
      std::printf("fills\n");
      std::printf("  %-12s %-4s %12s %12s %10s\n", "date", "side", "price",
                  "quantity", "commission");
      constexpr std::size_t kMaxPrinted = 50;
      const std::size_t printed = std::min(engine.fills().size(), kMaxPrinted);
      for (std::size_t index = 0; index < printed; ++index) {
        const auto& fill = engine.fills()[index];
        std::printf("  %-12s %-4s %12.4f %12.4f %10.2f\n",
                    format_timestamp(fill.timestamp).c_str(),
                    fill.side == Side::Buy ? "B" : "S", fill.price,
                    fill.quantity, fill.commission);
      }
      if (engine.fills().size() > printed) {
        std::printf("  ... %zu more fills\n", engine.fills().size() - printed);
      }
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    return 1;
  }

  return 0;
}
