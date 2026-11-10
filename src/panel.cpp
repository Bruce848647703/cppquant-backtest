#include "cppquant/backtest.hpp"

#include "validate.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cppquant {

BarPanel::BarPanel(std::vector<SymbolSeries> series) {
  if (series.empty()) {
    throw std::invalid_argument("bar panel requires at least one symbol");
  }

  std::unordered_set<std::string> seen;
  std::map<std::int64_t, std::vector<SymbolBar>> by_time;
  std::size_t total_bars = 0;

  for (const auto& symbol_series : series) {
    if (symbol_series.symbol.empty()) {
      throw std::invalid_argument("symbol names must not be empty");
    }
    if (!seen.insert(symbol_series.symbol).second) {
      throw std::invalid_argument("duplicate symbol in bar panel");
    }
    if (symbol_series.bars.empty()) {
      throw std::invalid_argument("each symbol must have at least one bar");
    }

    symbols_.push_back(symbol_series.symbol);
    for (std::size_t index = 0; index < symbol_series.bars.size(); ++index) {
      const Bar& bar = symbol_series.bars[index];
      if (index > 0 &&
          bar.timestamp <= symbol_series.bars[index - 1].timestamp) {
        throw std::invalid_argument(
            "bars for a symbol must have strictly increasing timestamps");
      }
      detail::validate_bar(bar, "symbol " + symbol_series.symbol + " bar " +
                                    std::to_string(index));
      by_time[bar.timestamp].push_back(SymbolBar{symbol_series.symbol, bar});
      ++total_bars;
    }
  }

  batches_.reserve(by_time.size());
  for (auto& [timestamp, bars] : by_time) {
    BarBatch batch;
    batch.timestamp = timestamp;
    batch.bars = std::move(bars);
    batches_.push_back(std::move(batch));
  }

  missing_bars_ = symbols_.size() * batches_.size() - total_bars;
  for (const auto& symbol : symbols_) {
    const auto iterator = std::find_if(
        series.begin(), series.end(),
        [&](const SymbolSeries& value) { return value.symbol == symbol; });
    missing_by_symbol_[symbol] = batches_.size() - iterator->bars.size();
  }
}

std::size_t BarPanel::missing_bars(const std::string& symbol) const {
  const auto iterator = missing_by_symbol_.find(symbol);
  return iterator == missing_by_symbol_.end() ? 0 : iterator->second;
}

void SingleSymbolBatchStrategy::on_batch(const BarBatch& batch,
                                         Backtester& engine) {
  const Bar* bar = batch.find(symbol_);
  if (bar == nullptr) {
    throw std::runtime_error("batch does not contain the adapter symbol " +
                             symbol_);
  }
  strategy_.on_bar(*bar, engine);
}

Performance Backtester::run_batch(const std::vector<BarBatch>& batches,
                                  const std::string& trade_symbol,
                                  BatchStrategy& strategy) {
  if (batches.empty()) {
    throw std::invalid_argument("batch backtest requires at least one batch");
  }
  if (trade_symbol.empty()) {
    throw std::invalid_argument("trade symbol must not be empty");
  }

  for (std::size_t index = 0; index < batches.size(); ++index) {
    const BarBatch& batch = batches[index];
    if (index > 0 && batch.timestamp <= batches[index - 1].timestamp) {
      throw std::invalid_argument(
          "batch timestamps must be strictly increasing");
    }
    if (batch.bars.empty()) {
      throw std::invalid_argument("a batch must contain at least one bar");
    }

    std::unordered_set<std::string> symbols;
    for (const auto& entry : batch.bars) {
      if (!symbols.insert(entry.symbol).second) {
        throw std::invalid_argument(
            "a batch must not contain duplicate symbols");
      }
      detail::validate_bar(entry.bar, "batch " + std::to_string(index) +
                                          " symbol " + entry.symbol);
    }

    if (!batch.contains(trade_symbol)) {
      throw std::invalid_argument("trade symbol " + trade_symbol +
                                  " is missing from batch " +
                                  std::to_string(index));
    }
  }

  reset();
  default_symbol_ = trade_symbol;

  for (std::size_t index = 0; index < batches.size(); ++index) {
    current_bar_ = static_cast<std::int64_t>(index);
    std::unordered_map<std::string, const Bar*> batch_bars;
    batch_bars.reserve(batches[index].bars.size());
    for (const auto& entry : batches[index].bars) {
      batch_bars[entry.symbol] = &entry.bar;
    }

    if (index > 0) {
      execute_pending(batch_bars);
    }
    strategy.on_batch(batches[index], *this);
    expire_day_orders();
    for (const auto& entry : batches[index].bars) {
      marks_[entry.symbol] = entry.bar.close;
    }
    equity_curve_.push_back(portfolio_equity());
    equity_timestamps_.push_back(batches[index].timestamp);
  }

  return summarize(batches.size());
}

} // namespace cppquant
