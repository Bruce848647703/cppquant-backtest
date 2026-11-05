#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
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

using OrderId = std::uint64_t;

enum class OrderType : std::uint8_t {
  Market,
  Limit,
  Stop,
  StopLimit,
};

enum class TimeInForce : std::uint8_t {
  GoodTilCancelled,
  Day,
};

enum class OrderStatus : std::uint8_t {
  Pending,
  PartiallyFilled,
  Filled,
  Cancelled,
  Expired,
};

enum class SlippageModel : std::uint8_t {
  FixedBps,
  Spread,
  Impact,
};

inline const char* to_string(SlippageModel model) noexcept {
  switch (model) {
  case SlippageModel::FixedBps:
    return "fixed-bps";
  case SlippageModel::Spread:
    return "spread";
  case SlippageModel::Impact:
    return "impact";
  }
  return "unknown";
}

struct OrderState {
  OrderId id{};
  Side side{Side::Buy};
  OrderType type{OrderType::Market};
  TimeInForce time_in_force{TimeInForce::GoodTilCancelled};
  double quantity{};
  double filled_quantity{};
  double limit_price{};
  double stop_price{};
  double average_fill_price{};
  OrderStatus status{OrderStatus::Pending};
  bool triggered{false};
  std::int64_t submitted_bar{-1};
  std::int64_t expires_after_bar{-1};
};

struct Fill {
  OrderId order_id{};
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
  /// Base adverse price movement in basis points applied to market and stop
  /// fills. Limit and stop-limit fills never trade worse than their limit.
  double slippage_bps{1.0};
  SlippageModel slippage_model{SlippageModel::FixedBps};
  /// Full spread in basis points; the Spread model applies half of it.
  double spread_bps{0.0};
  /// Impact coefficient in basis points at 100% of bar volume. The Impact
  /// model uses base slippage plus impact_bps * sqrt(participation).
  double impact_bps{0.0};
  /// Maximum fraction of a bar's volume that can fill across all orders.
  double max_volume_participation{0.1};
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

/// Strategy callback. `on_bar` is invoked after the current bar's open/high/
/// low have been used to execute orders submitted on the previous bar. Orders
/// submitted here become eligible on the next bar, which prevents look-ahead
/// bias.
class Strategy {
public:
  virtual ~Strategy() = default;
  virtual void on_bar(const Bar& bar, Backtester& engine) = 0;
};

/// Single-instrument event-driven backtester.
///
/// Market, limit, stop and stop-limit orders are submitted from a strategy and
/// matched against the next bar's OHLC. The engine supports long and short
/// positions, day orders and good-til-cancelled orders, but does not simulate
/// margin calls or borrow costs.
class Backtester {
public:
  explicit Backtester(BacktestConfig config = {});

  /// Run `strategy` over `bars` and return the performance summary. Calling
  /// run again resets all state.
  Performance run(const std::vector<Bar>& bars, Strategy& strategy);

  /// Market order entry, callable from Strategy::on_bar.
  OrderId buy(double quantity, TimeInForce tif = TimeInForce::GoodTilCancelled);
  OrderId sell(double quantity,
               TimeInForce tif = TimeInForce::GoodTilCancelled);

  OrderId buy_limit(double quantity, double limit_price,
                    TimeInForce tif = TimeInForce::GoodTilCancelled);
  OrderId sell_limit(double quantity, double limit_price,
                     TimeInForce tif = TimeInForce::GoodTilCancelled);

  OrderId buy_stop(double quantity, double stop_price,
                   TimeInForce tif = TimeInForce::GoodTilCancelled);
  OrderId sell_stop(double quantity, double stop_price,
                    TimeInForce tif = TimeInForce::GoodTilCancelled);

  OrderId buy_stop_limit(double quantity, double stop_price, double limit_price,
                         TimeInForce tif = TimeInForce::GoodTilCancelled);
  OrderId sell_stop_limit(double quantity, double stop_price,
                          double limit_price,
                          TimeInForce tif = TimeInForce::GoodTilCancelled);

  /// Cancel a pending order. Returns false for unknown or terminal orders.
  bool cancel_order(OrderId id);

  /// Look up an order, including filled, cancelled and expired orders. The
  /// returned pointer is valid until the next `run()`.
  const OrderState* find_order(OrderId id) const noexcept;

  /// Copy of all orders that are still pending or partially filled.
  std::vector<OrderState> open_orders() const;

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
  OrderId submit_order(OrderType type, Side side, double quantity,
                       double limit_price, double stop_price, TimeInForce tif);
  void execute_pending(const Bar& bar);
  bool try_fill(OrderState& order, const Bar& bar, double& price,
                bool& triggered_now) const;
  void apply_fill(OrderState& order, double price, std::int64_t timestamp,
                  double quantity);
  double effective_slippage_bps(const Bar& bar, double quantity) const;
  void expire_day_orders();
  void remove_pending(OrderId id);
  void reset();

  BacktestConfig config_;
  double cash_{};
  double position_{};
  double average_cost_{};
  double realized_pnl_{};
  double total_commission_{};
  double total_notional_{};
  OrderId next_order_id_{1};
  std::size_t winning_closes_{};
  std::size_t losing_closes_{};
  std::int64_t current_bar_{-1};

  std::unordered_map<OrderId, OrderState> orders_;
  std::vector<OrderId> pending_;
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
