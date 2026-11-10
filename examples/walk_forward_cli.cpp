#include "cppquant/walk_forward.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using namespace cppquant;

namespace {

std::vector<int> parse_list(const std::string& text, const char* flag) {
  std::vector<int> values;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    const std::size_t comma = text.find(',', begin);
    const std::string token = text.substr(
        begin, comma == std::string::npos ? std::string::npos : comma - begin);
    if (token.empty()) {
      throw std::invalid_argument(std::string("empty value in ") + flag);
    }
    values.push_back(std::stoi(token));
    if (comma == std::string::npos) {
      break;
    }
    begin = comma + 1;
  }
  if (values.empty()) {
    throw std::invalid_argument(std::string("missing values for ") + flag);
  }
  return values;
}

double next_value(int& index, int argc, char** argv, const char* flag) {
  if (index + 1 >= argc) {
    throw std::invalid_argument(std::string("missing value for ") + flag);
  }
  return std::stod(argv[++index]);
}

std::string next_text(int& index, int argc, char** argv, const char* flag) {
  if (index + 1 >= argc) {
    throw std::invalid_argument(std::string("missing value for ") + flag);
  }
  return argv[++index];
}

void usage(const char* program) {
  std::printf(
      "usage: %s --csv PATH [options]\n"
      "\n"
      "  --csv PATH          OHLCV CSV file (required)\n"
      "  --train N           training bars per window (default 252)\n"
      "  --test N            test bars per window (default 63)\n"
      "  --step N            bars between windows (default test length)\n"
      "  --fast LIST         comma-separated fast windows (default 5,10)\n"
      "  --slow LIST         comma-separated slow windows (default 20,30)\n"
      "  --quantity N        order quantity (default 100)\n"
      "  --export PATH       write per-window results CSV\n"
      "  --help              show this message\n",
      program);
}

} // namespace

int main(int argc, char** argv) {
  try {
    std::string csv_path;
    std::vector<int> fast{5, 10};
    std::vector<int> slow{20, 30};
    std::string export_path;
    WalkForwardConfig config;

    for (int index = 1; index < argc; ++index) {
      const std::string flag = argv[index];
      if (flag == "--help" || flag == "-h") {
        usage(argv[0]);
        return 0;
      } else if (flag == "--csv") {
        csv_path = next_text(index, argc, argv, "--csv");
      } else if (flag == "--train") {
        config.train_bars =
            static_cast<std::size_t>(next_value(index, argc, argv, "--train"));
      } else if (flag == "--test") {
        config.test_bars =
            static_cast<std::size_t>(next_value(index, argc, argv, "--test"));
      } else if (flag == "--step") {
        config.step_bars =
            static_cast<std::size_t>(next_value(index, argc, argv, "--step"));
      } else if (flag == "--fast") {
        fast = parse_list(next_text(index, argc, argv, "--fast"), "--fast");
      } else if (flag == "--slow") {
        slow = parse_list(next_text(index, argc, argv, "--slow"), "--slow");
      } else if (flag == "--quantity") {
        config.quantity = next_value(index, argc, argv, "--quantity");
      } else if (flag == "--export") {
        export_path = next_text(index, argc, argv, "--export");
      } else {
        throw std::invalid_argument("unknown argument: " + flag);
      }
    }

    if (csv_path.empty()) {
      throw std::invalid_argument("--csv is required");
    }

    const auto bars = load_bars_csv(csv_path);
    const auto grid = make_sma_grid(fast, slow);
    const auto result = run_walk_forward(bars, grid, config);

    std::printf("cppquant-backtest walk-forward\n");
    std::printf("bars: %zu, train: %zu, test: %zu, grid: %zu\n", bars.size(),
                config.train_bars, config.test_bars, grid.size());
    std::printf("\nwindows\n");
    std::printf("  %3s  %10s  %8s  %8s  %8s  %8s\n", "#", "train", "test",
                "fast", "slow", "test_ret");
    for (std::size_t index = 0; index < result.windows.size(); ++index) {
      const auto& window = result.windows[index];
      std::printf("  %3zu  [%3zu,%3zu)  [%3zu,%3zu)  %8d  %8d  %7.2f%%\n",
                  index + 1, window.train_start, window.train_end,
                  window.test_start, window.test_end,
                  window.parameters.fast_window, window.parameters.slow_window,
                  window.testing.total_return * 100.0);
    }

    std::printf("\naggregate OOS\n");
    std::printf("  initial equity      %14.2f\n", result.initial_equity);
    std::printf("  final equity        %14.2f\n", result.final_equity);
    std::printf("  total return        %13.2f%%\n",
                result.total_return * 100.0);
    std::printf("  annualized return   %13.2f%%\n",
                result.annualized_return * 100.0);
    std::printf("  annualized vol      %13.2f%%\n",
                result.annualized_volatility * 100.0);
    std::printf("  sharpe ratio        %14.3f\n", result.sharpe_ratio);
    std::printf("  max drawdown        %13.2f%%\n",
                result.max_drawdown * 100.0);
    std::printf("  trades              %14zu\n", result.trades);

    if (!export_path.empty()) {
      write_walk_forward_csv(result, export_path);
      std::printf("exported %s\n", export_path.c_str());
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    return 1;
  }

  return 0;
}
