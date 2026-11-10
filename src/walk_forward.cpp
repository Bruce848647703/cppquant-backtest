#include "cppquant/walk_forward.hpp"

#include "validate.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace cppquant {
namespace {

double aggregate_metric(const Performance& performance, bool by_sharpe) {
  const double value =
      by_sharpe ? performance.sharpe_ratio : performance.total_return;
  return std::isfinite(value) ? value
                              : -std::numeric_limits<double>::infinity();
}

void compute_aggregate(WalkForwardResult& result,
                       const BacktestConfig& config) {
  result.initial_equity = config.initial_cash;
  if (result.equity_curve.empty()) {
    result.final_equity = result.initial_equity;
    return;
  }

  result.bars = result.equity_curve.size();
  result.final_equity = result.equity_curve.back();
  result.total_return = result.final_equity / result.initial_equity - 1.0;

  double peak = result.equity_curve.front();
  for (const double value : result.equity_curve) {
    peak = std::max(peak, value);
    if (peak > 0.0) {
      result.max_drawdown =
          std::max(result.max_drawdown, (peak - value) / peak);
    }
  }

  if (result.equity_curve.size() <= 1) {
    return;
  }

  std::vector<double> returns;
  returns.reserve(result.equity_curve.size() - 1);
  for (std::size_t index = 1; index < result.equity_curve.size(); ++index) {
    if (result.equity_curve[index - 1] != 0.0) {
      returns.push_back(
          result.equity_curve[index] / result.equity_curve[index - 1] - 1.0);
    }
  }
  if (returns.empty()) {
    return;
  }

  const double mean = std::accumulate(returns.begin(), returns.end(), 0.0) /
                      static_cast<double>(returns.size());
  double variance = 0.0;
  for (const double value : returns) {
    const double deviation = value - mean;
    variance += deviation * deviation;
  }
  if (returns.size() > 1) {
    variance /= static_cast<double>(returns.size() - 1);
  } else {
    variance = 0.0;
  }
  const double deviation = std::sqrt(variance);
  result.annualized_volatility = deviation * std::sqrt(config.periods_per_year);
  if (deviation > 0.0) {
    const double excess =
        mean - config.risk_free_rate / config.periods_per_year;
    result.sharpe_ratio =
        excess / deviation * std::sqrt(config.periods_per_year);
  }
  if (result.initial_equity > 0.0 && result.final_equity > 0.0) {
    result.annualized_return =
        std::pow(result.final_equity / result.initial_equity,
                 config.periods_per_year /
                     static_cast<double>(returns.size())) -
        1.0;
  }
}

} // namespace

WalkForwardResult run_walk_forward(const std::vector<Bar>& bars,
                                   const std::vector<SmaParameters>& grid,
                                   const WalkForwardConfig& config) {
  if (bars.empty()) {
    throw std::invalid_argument("walk-forward requires at least one bar");
  }
  for (std::size_t index = 0; index < bars.size(); ++index) {
    detail::validate_bar(bars[index], "bar " + std::to_string(index));
  }
  if (grid.empty()) {
    throw std::invalid_argument("walk-forward requires a parameter grid");
  }
  if (config.train_bars == 0 || config.test_bars == 0) {
    throw std::invalid_argument("train and test windows must be positive");
  }
  const std::size_t step =
      config.step_bars == 0 ? config.test_bars : config.step_bars;
  if (step < config.test_bars) {
    throw std::invalid_argument(
        "step must be at least the test window to avoid overlap");
  }
  if (!std::isfinite(config.quantity) || config.quantity <= 0.0) {
    throw std::invalid_argument("walk-forward quantity must be positive");
  }
  for (const auto& parameters : grid) {
    if (parameters.fast_window < 1 || parameters.slow_window < 1 ||
        parameters.fast_window >= parameters.slow_window) {
      throw std::invalid_argument(
          "walk-forward grid requires positive fast < slow");
    }
  }

  WalkForwardResult result;
  double equity = config.backtest.initial_cash;
  std::size_t start = 0;

  while (start + config.train_bars <= bars.size()) {
    const std::size_t train_start = start;
    const std::size_t train_end = start + config.train_bars;
    const std::size_t test_start = train_end;
    const std::size_t test_end =
        std::min(test_start + config.test_bars, bars.size());
    if (test_start >= test_end) {
      break;
    }

    const std::vector<Bar> train_bars(
        bars.begin() + static_cast<std::ptrdiff_t>(train_start),
        bars.begin() + static_cast<std::ptrdiff_t>(train_end));
    const std::vector<Bar> test_bars(
        bars.begin() + static_cast<std::ptrdiff_t>(test_start),
        bars.begin() + static_cast<std::ptrdiff_t>(test_end));

    SmaSweepConfig sweep_config;
    sweep_config.backtest = config.backtest;
    sweep_config.backtest.initial_cash = equity;
    sweep_config.quantity = config.quantity;
    sweep_config.threads = config.threads;
    const auto sweep = run_sma_sweep(train_bars, grid, sweep_config);

    SmaParameters best = sweep.front().parameters;
    Performance best_training = sweep.front().performance;
    double best_metric =
        aggregate_metric(best_training, config.select_by_sharpe);
    for (std::size_t index = 1; index < sweep.size(); ++index) {
      const double metric =
          aggregate_metric(sweep[index].performance, config.select_by_sharpe);
      if (metric > best_metric) {
        best = sweep[index].parameters;
        best_training = sweep[index].performance;
        best_metric = metric;
      }
    }

    BacktestConfig backtest_config = config.backtest;
    backtest_config.initial_cash = equity;
    Backtester tester(backtest_config);
    SmaCrossoverStrategy selected(best.fast_window, best.slow_window,
                                  config.quantity);
    const Performance testing = tester.run(test_bars, selected);

    result.equity_curve.insert(result.equity_curve.end(),
                               tester.equity_curve().begin(),
                               tester.equity_curve().end());
    result.timestamps.insert(result.timestamps.end(),
                             tester.equity_timestamps().begin(),
                             tester.equity_timestamps().end());
    result.trades += testing.trades;
    equity = tester.equity_curve().back();

    WalkForwardWindow window;
    window.train_start = train_start;
    window.train_end = train_end;
    window.test_start = test_start;
    window.test_end = test_end;
    window.parameters = best;
    window.training = best_training;
    window.testing = testing;
    result.windows.push_back(window);

    start += step;
  }

  compute_aggregate(result, config.backtest);
  return result;
}

void write_walk_forward_csv(const WalkForwardResult& result,
                            const std::string& path) {
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("cannot open " + path);
  }

  out << "window,train_start,train_end,test_start,test_end,fast,slow,"
         "train_return,train_sharpe,test_return,test_sharpe,test_trades\n";
  out << std::setprecision(12);
  for (std::size_t index = 0; index < result.windows.size(); ++index) {
    const auto& window = result.windows[index];
    out << index + 1 << ',' << window.train_start << ',' << window.train_end
        << ',' << window.test_start << ',' << window.test_end << ','
        << window.parameters.fast_window << ',' << window.parameters.slow_window
        << ',' << window.training.total_return << ','
        << window.training.sharpe_ratio << ',' << window.testing.total_return
        << ',' << window.testing.sharpe_ratio << ',' << window.testing.trades
        << '\n';
  }
}

} // namespace cppquant
