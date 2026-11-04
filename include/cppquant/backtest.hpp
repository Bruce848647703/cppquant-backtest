#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace cppquant {

/// One OHLCV bar. `timestamp` is an arbitrary integer, typically epoch
/// seconds; it is used for reporting and for the annualization of returns.
struct Bar {
  std::int64_t timestamp{};
  double open{};
  double high{};
  double low{};
  double close{};
  double volume{};
};

enum class Side : std::uint8_t { Buy, Sell };

struct Fill {
  std::uint64_t order_id{};
  std::int64_t timestamp{};
  Side side{Side::Buy};
  double price{};
  double quantity{};
  double commission{};
};

struct BacktestConfig {
  double initial_cash{100'000.0};
  /// Commission in basis points of notional, charged on both sides.
  double commission_bps{1.0};
  /// Adverse price movement in basis points, applied to the open price.
  double slippage_bps{1.0};
  /// Number of bars per year used for Sharpe and annualized returns.
  double periods_per_year{252.0};
  /// Annual risk-free rate used for the Sharpe ratio.
  double risk_free_rate{0.0};
};

struct Performance {
  double initial_equity{};
  double final_equity{};
  double total_return{};
  double annualized_return{};
  double annualized_volatility{};
  double sharpe_ratio{};
  double max_drawdown{};
  double turnover{};
  double win_rate{};
  double total_commission{};
  double realized_pnl{};
  double final_position{};
  std::size_t bars{};
  std::size_t trades{};
  std::size_t winning_trades{};
  std::size_t losing_trades{};
};

class Backtester;

/// Strategy callback. `on_bar` is invoked after the bar's open has been used
/// to execute orders submitted on the previous bar. Orders submitted here
/// execute at the next bar's open, which prevents look-ahead bias.
class Strategy {
public:
  virtual ~Strategy() = default;
  virtual void on_bar(const Bar& bar, Backtester& engine) = 0;
};

/// Single-instrument event-driven backtester.
///
/// Orders are market orders submitted from a strategy and executed at the next
/// bar's open with the configured slippage and commission. The engine supports
/// long and short positions but does not simulate margin calls or borrow
/// costs.
class Backtester {
public:
  explicit Backtester(BacktestConfig config = {});

  /// Run `strategy` over `bars` and return the performance summary. Calling
  /// run again resets all state.
  Performance run(const std::vector<Bar>& bars, Strategy& strategy);

  /// Order entry, callable from Strategy::on_bar.
  void buy(double quantity);
  void sell(double quantity);
  /// Move to `target_position` (positive long, negative short) using the
  /// current position plus pending orders as the starting point.
  void order_target(double target_position);
  void close_position();

  double position() const noexcept {
    return position_;
  }
  double cash() const noexcept {
    return cash_;
  }
  double average_cost() const noexcept {
    return average_cost_;
  }
  double realized_pnl() const noexcept {
    return realized_pnl_;
  }
  double equity(double mark_price) const noexcept {
    return cash_ + position_ * mark_price;
  }

  bool has_pending_orders() const noexcept {
    return !pending_.empty();
  }
  std::size_t pending_order_count() const noexcept {
    return pending_.size();
  }

  const std::vector<Fill>& fills() const noexcept {
    return fills_;
  }
  const std::vector<double>& equity_curve() const noexcept {
    return equity_curve_;
  }
  const std::vector<std::int64_t>& equity_timestamps() const noexcept {
    return equity_timestamps_;
  }
  const BacktestConfig& config() const noexcept {
    return config_;
  }

private:
  struct PendingOrder {
    std::uint64_t id{};
    Side side{Side::Buy};
    double quantity{};
  };

  void execute_pending(double open_price, std::int64_t timestamp);
  void apply_fill(const PendingOrder& order, double price,
                  std::int64_t timestamp);
  void reset();

  BacktestConfig config_;
  double cash_{};
  double position_{};
  double average_cost_{};
  double realized_pnl_{};
  double total_commission_{};
  double total_notional_{};
  std::uint64_t next_order_id_{1};
  std::size_t winning_closes_{};
  std::size_t losing_closes_{};

  std::vector<PendingOrder> pending_;
  std::vector<Fill> fills_;
  std::vector<double> equity_curve_;
  std::vector<std::int64_t> equity_timestamps_;
};

/// Load a `timestamp,open,high,low,close,volume` CSV. Blank lines and lines
/// starting with `#` are ignored; a header line is skipped. Throws
/// std::runtime_error on I/O or parse errors.
std::vector<Bar> load_bars_csv(const std::string& path);

/// Long-only or long/short moving-average crossover.
class SmaCrossoverStrategy : public Strategy {
public:
  SmaCrossoverStrategy(int fast_window, int slow_window, double quantity,
                       bool allow_short = false);

  void on_bar(const Bar& bar, Backtester& engine) override;

  int fast_window() const noexcept {
    return fast_window_;
  }
  int slow_window() const noexcept {
    return slow_window_;
  }

private:
  int fast_window_{};
  int slow_window_{};
  double quantity_{};
  bool allow_short_{};
  std::deque<double> closes_;
  double slow_sum_{};
};

/// Buy a fixed quantity on the first bar and hold it.
class BuyAndHoldStrategy : public Strategy {
public:
  explicit BuyAndHoldStrategy(double quantity);

  void on_bar(const Bar& bar, Backtester& engine) override;

private:
  double quantity_{};
  bool submitted_{};
};

} // namespace cppquant
