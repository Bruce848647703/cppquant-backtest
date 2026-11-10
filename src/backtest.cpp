#include "cppquant/backtest.hpp"

#include "validate.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace cppquant {

Backtester::Backtester(BacktestConfig config) : config_(config) {
  if (!std::isfinite(config_.initial_cash) || config_.initial_cash <= 0.0) {
    throw std::invalid_argument("initial cash must be positive");
  }
  if (!std::isfinite(config_.commission_bps) || config_.commission_bps < 0.0) {
    throw std::invalid_argument("commission must be non-negative");
  }
  if (!std::isfinite(config_.slippage_bps) || config_.slippage_bps < 0.0) {
    throw std::invalid_argument("slippage must be non-negative");
  }
  if (!std::isfinite(config_.spread_bps) || config_.spread_bps < 0.0) {
    throw std::invalid_argument("spread must be non-negative");
  }
  if (!std::isfinite(config_.impact_bps) || config_.impact_bps < 0.0) {
    throw std::invalid_argument("impact must be non-negative");
  }
  if (!std::isfinite(config_.max_volume_participation) ||
      config_.max_volume_participation <= 0.0 ||
      config_.max_volume_participation > 1.0) {
    throw std::invalid_argument("volume participation must be in (0, 1]");
  }
  if (!std::isfinite(config_.periods_per_year) ||
      config_.periods_per_year <= 0.0) {
    throw std::invalid_argument("periods per year must be positive");
  }
  if (!std::isfinite(config_.risk_free_rate)) {
    throw std::invalid_argument("risk-free rate must be finite");
  }
  reset();
}

void Backtester::reset() {
  cash_ = config_.initial_cash;
  total_commission_ = 0.0;
  total_notional_ = 0.0;
  next_order_id_ = 1;
  current_bar_ = -1;
  default_symbol_.clear();
  positions_.clear();
  marks_.clear();
  orders_.clear();
  pending_.clear();
  fills_.clear();
  equity_curve_.clear();
  equity_timestamps_.clear();
}

OrderId Backtester::submit_order(const std::string& symbol, OrderType type,
                                 Side side, double quantity, double limit_price,
                                 double stop_price, TimeInForce tif) {
  if (!std::isfinite(quantity) || quantity <= 0.0) {
    throw std::invalid_argument("order quantity must be positive and finite");
  }
  if ((type == OrderType::Limit || type == OrderType::StopLimit) &&
      (!std::isfinite(limit_price) || limit_price <= 0.0)) {
    throw std::invalid_argument("limit price must be positive and finite");
  }
  if ((type == OrderType::Stop || type == OrderType::StopLimit) &&
      (!std::isfinite(stop_price) || stop_price <= 0.0)) {
    throw std::invalid_argument("stop price must be positive and finite");
  }

  OrderState order;
  order.id = next_order_id_++;
  order.side = side;
  order.type = type;
  order.time_in_force = tif;
  order.quantity = quantity;
  order.limit_price = type == OrderType::Limit || type == OrderType::StopLimit
                          ? limit_price
                          : 0.0;
  order.stop_price = type == OrderType::Stop || type == OrderType::StopLimit
                         ? stop_price
                         : 0.0;
  order.submitted_bar = current_bar_;
  order.expires_after_bar = tif == TimeInForce::Day ? current_bar_ + 1 : -1;
  order.status = OrderStatus::Pending;
  order.symbol = symbol;

  const OrderId id = order.id;
  orders_.emplace(id, order);
  pending_.push_back(id);
  return id;
}

OrderId Backtester::buy(double quantity, TimeInForce tif) {
  return buy(default_symbol_, quantity, tif);
}

OrderId Backtester::buy(const std::string& symbol, double quantity,
                        TimeInForce tif) {
  return submit_order(symbol, OrderType::Market, Side::Buy, quantity, 0.0, 0.0,
                      tif);
}

OrderId Backtester::sell(double quantity, TimeInForce tif) {
  return sell(default_symbol_, quantity, tif);
}

OrderId Backtester::sell(const std::string& symbol, double quantity,
                         TimeInForce tif) {
  return submit_order(symbol, OrderType::Market, Side::Sell, quantity, 0.0, 0.0,
                      tif);
}

OrderId Backtester::buy_limit(double quantity, double limit_price,
                              TimeInForce tif) {
  return buy_limit(default_symbol_, quantity, limit_price, tif);
}

OrderId Backtester::buy_limit(const std::string& symbol, double quantity,
                              double limit_price, TimeInForce tif) {
  return submit_order(symbol, OrderType::Limit, Side::Buy, quantity,
                      limit_price, 0.0, tif);
}

OrderId Backtester::sell_limit(double quantity, double limit_price,
                               TimeInForce tif) {
  return sell_limit(default_symbol_, quantity, limit_price, tif);
}

OrderId Backtester::sell_limit(const std::string& symbol, double quantity,
                               double limit_price, TimeInForce tif) {
  return submit_order(symbol, OrderType::Limit, Side::Sell, quantity,
                      limit_price, 0.0, tif);
}

OrderId Backtester::buy_stop(double quantity, double stop_price,
                             TimeInForce tif) {
  return buy_stop(default_symbol_, quantity, stop_price, tif);
}

OrderId Backtester::buy_stop(const std::string& symbol, double quantity,
                             double stop_price, TimeInForce tif) {
  return submit_order(symbol, OrderType::Stop, Side::Buy, quantity, 0.0,
                      stop_price, tif);
}

OrderId Backtester::sell_stop(double quantity, double stop_price,
                              TimeInForce tif) {
  return sell_stop(default_symbol_, quantity, stop_price, tif);
}

OrderId Backtester::sell_stop(const std::string& symbol, double quantity,
                              double stop_price, TimeInForce tif) {
  return submit_order(symbol, OrderType::Stop, Side::Sell, quantity, 0.0,
                      stop_price, tif);
}

OrderId Backtester::buy_stop_limit(double quantity, double stop_price,
                                   double limit_price, TimeInForce tif) {
  return buy_stop_limit(default_symbol_, quantity, stop_price, limit_price,
                        tif);
}

OrderId Backtester::buy_stop_limit(const std::string& symbol, double quantity,
                                   double stop_price, double limit_price,
                                   TimeInForce tif) {
  return submit_order(symbol, OrderType::StopLimit, Side::Buy, quantity,
                      limit_price, stop_price, tif);
}

OrderId Backtester::sell_stop_limit(double quantity, double stop_price,
                                    double limit_price, TimeInForce tif) {
  return sell_stop_limit(default_symbol_, quantity, stop_price, limit_price,
                         tif);
}

OrderId Backtester::sell_stop_limit(const std::string& symbol, double quantity,
                                    double stop_price, double limit_price,
                                    TimeInForce tif) {
  return submit_order(symbol, OrderType::StopLimit, Side::Sell, quantity,
                      limit_price, stop_price, tif);
}

bool Backtester::cancel_order(OrderId id) {
  auto iterator = orders_.find(id);
  if (iterator == orders_.end()) {
    return false;
  }
  OrderState& order = iterator->second;
  if (order.status != OrderStatus::Pending &&
      order.status != OrderStatus::PartiallyFilled) {
    return false;
  }
  order.status = OrderStatus::Cancelled;
  remove_pending(id);
  return true;
}

const OrderState* Backtester::find_order(OrderId id) const noexcept {
  auto iterator = orders_.find(id);
  return iterator == orders_.end() ? nullptr : &iterator->second;
}

std::vector<OrderState> Backtester::open_orders() const {
  std::vector<OrderState> result;
  result.reserve(pending_.size());
  for (const OrderId id : pending_) {
    auto iterator = orders_.find(id);
    if (iterator != orders_.end()) {
      result.push_back(iterator->second);
    }
  }
  return result;
}

void Backtester::order_target(double target_position) {
  order_target(default_symbol_, target_position);
}

void Backtester::order_target(const std::string& symbol,
                              double target_position) {
  if (!std::isfinite(target_position)) {
    throw std::invalid_argument("target position must be finite");
  }

  double pending_quantity = 0.0;
  for (const OrderId id : pending_) {
    auto iterator = orders_.find(id);
    if (iterator == orders_.end()) {
      continue;
    }
    const OrderState& order = iterator->second;
    if (order.symbol != symbol) {
      continue;
    }
    const double remaining = order.quantity - order.filled_quantity;
    pending_quantity += order.side == Side::Buy ? remaining : -remaining;
  }

  const double delta = target_position - (position(symbol) + pending_quantity);
  if (delta > 1e-12) {
    buy(symbol, delta);
  } else if (delta < -1e-12) {
    sell(symbol, -delta);
  }
}

void Backtester::close_position() {
  order_target(default_symbol_, 0.0);
}

void Backtester::close_position(const std::string& symbol) {
  order_target(symbol, 0.0);
}

double Backtester::position(const std::string& symbol) const noexcept {
  const auto iterator = positions_.find(symbol);
  return iterator == positions_.end() ? 0.0 : iterator->second.quantity;
}

double Backtester::average_cost(const std::string& symbol) const noexcept {
  const auto iterator = positions_.find(symbol);
  return iterator == positions_.end() ? 0.0 : iterator->second.average_cost;
}

double Backtester::realized_pnl(const std::string& symbol) const noexcept {
  const auto iterator = positions_.find(symbol);
  return iterator == positions_.end() ? 0.0 : iterator->second.realized_pnl;
}

double Backtester::portfolio_equity() const noexcept {
  double equity = cash_;
  for (const auto& [symbol, position] : positions_) {
    const auto mark = marks_.find(symbol);
    const double price =
        mark == marks_.end() ? position.average_cost : mark->second;
    equity += position.quantity * price;
  }
  return equity;
}

bool Backtester::try_fill(OrderState& order, const Bar& bar, double& price,
                          bool& triggered_now) const {
  triggered_now = false;
  const bool is_buy = order.side == Side::Buy;

  auto limit_fill = [&]() {
    if (is_buy) {
      if (bar.open <= order.limit_price) {
        price = bar.open;
        return true;
      }
      if (bar.low <= order.limit_price) {
        price = order.limit_price;
        return true;
      }
    } else {
      if (bar.open >= order.limit_price) {
        price = bar.open;
        return true;
      }
      if (bar.high >= order.limit_price) {
        price = order.limit_price;
        return true;
      }
    }
    return false;
  };

  switch (order.type) {
  case OrderType::Market:
    price = bar.open;
    return true;
  case OrderType::Limit:
    return limit_fill();
  case OrderType::Stop:
    if (is_buy) {
      if (bar.open >= order.stop_price) {
        price = bar.open;
        return true;
      }
      if (bar.high >= order.stop_price) {
        price = order.stop_price;
        return true;
      }
    } else {
      if (bar.open <= order.stop_price) {
        price = bar.open;
        return true;
      }
      if (bar.low <= order.stop_price) {
        price = order.stop_price;
        return true;
      }
    }
    return false;
  case OrderType::StopLimit:
    if (!order.triggered) {
      const bool hit =
          is_buy
              ? (bar.open >= order.stop_price || bar.high >= order.stop_price)
              : (bar.open <= order.stop_price || bar.low <= order.stop_price);
      if (!hit) {
        return false;
      }
      triggered_now = true;
      // A gap through the stop activates the order at the open. A trigger
      // that only happens inside the bar is deliberately not filled in the
      // same bar: OHLC data cannot tell us whether the limit was reached
      // before or after the stop.
      if (is_buy) {
        if (bar.open >= order.stop_price && bar.open <= order.limit_price) {
          price = bar.open;
          return true;
        }
      } else {
        if (bar.open <= order.stop_price && bar.open >= order.limit_price) {
          price = bar.open;
          return true;
        }
      }
      return false;
    }
    return limit_fill();
  }
  return false;
}

void Backtester::apply_fill(OrderState& order, double price,
                            std::int64_t timestamp, double quantity) {
  if (!(quantity > 0.0)) {
    return;
  }

  const double signed_quantity = order.side == Side::Buy ? quantity : -quantity;
  const double notional = price * quantity;
  const double commission = notional * config_.commission_bps / 10'000.0;

  cash_ -= signed_quantity * price + commission;
  total_commission_ += commission;
  total_notional_ += notional;

  Position& position = position_for(order.symbol);
  const double new_position = position.quantity + signed_quantity;

  if (std::abs(position.quantity) < 1e-12) {
    position.average_cost = price;
  } else if ((position.quantity > 0.0) == (signed_quantity > 0.0)) {
    position.average_cost =
        (position.average_cost * position.quantity + price * signed_quantity) /
        new_position;
  } else {
    const double closing =
        std::min(std::abs(position.quantity), std::abs(signed_quantity));
    const double direction = position.quantity > 0.0 ? 1.0 : -1.0;
    const double pnl = (price - position.average_cost) * closing * direction;
    position.realized_pnl += pnl;
    if (pnl > 0.0) {
      ++position.winning_closes;
    } else if (pnl < 0.0) {
      ++position.losing_closes;
    }

    if (std::abs(new_position) < 1e-12) {
      position.average_cost = 0.0;
    } else if ((new_position > 0.0) != (position.quantity > 0.0)) {
      position.average_cost = price;
    }
  }

  position.quantity = new_position;
  if (std::abs(position.quantity) < 1e-12) {
    position.quantity = 0.0;
    position.average_cost = 0.0;
  }

  const double previous_notional =
      order.average_fill_price * order.filled_quantity;
  order.filled_quantity += quantity;
  order.average_fill_price =
      (previous_notional + price * quantity) / order.filled_quantity;
  if (order.filled_quantity + 1e-12 >= order.quantity) {
    order.status = OrderStatus::Filled;
  } else {
    order.status = OrderStatus::PartiallyFilled;
  }

  fills_.push_back(Fill{order.id, timestamp, order.side, price, quantity,
                        commission, order.symbol});
}

void Backtester::remove_pending(OrderId id) {
  pending_.erase(std::remove(pending_.begin(), pending_.end(), id),
                 pending_.end());
}

double Backtester::effective_slippage_bps(const Bar& bar,
                                          double quantity) const {
  switch (config_.slippage_model) {
  case SlippageModel::FixedBps:
    return config_.slippage_bps;
  case SlippageModel::Spread:
    return 0.5 * config_.spread_bps;
  case SlippageModel::Impact: {
    const double participation =
        bar.volume > 0.0 ? std::clamp(quantity / bar.volume, 0.0, 1.0) : 1.0;
    return config_.slippage_bps + config_.impact_bps * std::sqrt(participation);
  }
  }
  return config_.slippage_bps;
}

void Backtester::execute_pending(const Bar& bar) {
  std::vector<OrderId> still_pending;
  still_pending.reserve(pending_.size());
  double available =
      bar.volume > 0.0 ? bar.volume * config_.max_volume_participation : 0.0;

  for (const OrderId id : pending_) {
    auto iterator = orders_.find(id);
    if (iterator == orders_.end()) {
      continue;
    }
    OrderState& order = iterator->second;
    if (order.status != OrderStatus::Pending &&
        order.status != OrderStatus::PartiallyFilled) {
      continue;
    }
    if (order.symbol != default_symbol_) {
      still_pending.push_back(id);
      continue;
    }

    double price = 0.0;
    bool triggered_now = false;
    if (!try_fill(order, bar, price, triggered_now)) {
      if (triggered_now) {
        order.triggered = true;
      }
      still_pending.push_back(id);
      continue;
    }
    if (triggered_now) {
      order.triggered = true;
    }

    const double remaining = order.quantity - order.filled_quantity;
    const double quantity = std::min(remaining, available);
    if (quantity <= 1e-12) {
      still_pending.push_back(id);
      continue;
    }

    if (order.type == OrderType::Market || order.type == OrderType::Stop) {
      const double slippage = effective_slippage_bps(bar, quantity) / 10'000.0;
      price = order.side == Side::Buy ? price * (1.0 + slippage)
                                      : price * (1.0 - slippage);
      if (!std::isfinite(price) || price <= 0.0) {
        throw std::runtime_error("execution price is invalid");
      }
    }

    apply_fill(order, price, bar.timestamp, quantity);
    available -= quantity;
    if (order.status == OrderStatus::Pending ||
        order.status == OrderStatus::PartiallyFilled) {
      still_pending.push_back(id);
    }
  }

  pending_ = std::move(still_pending);
}

void Backtester::execute_pending(
    const std::unordered_map<std::string, const Bar*>& bars) {
  std::vector<OrderId> still_pending;
  still_pending.reserve(pending_.size());

  std::unordered_map<std::string, double> liquidity;
  liquidity.reserve(bars.size());
  for (const auto& [symbol, bar] : bars) {
    liquidity[symbol] = bar->volume > 0.0
                            ? bar->volume * config_.max_volume_participation
                            : 0.0;
  }

  for (const OrderId id : pending_) {
    auto iterator = orders_.find(id);
    if (iterator == orders_.end()) {
      continue;
    }
    OrderState& order = iterator->second;
    if (order.status != OrderStatus::Pending &&
        order.status != OrderStatus::PartiallyFilled) {
      continue;
    }

    const auto bar_iterator = bars.find(order.symbol);
    if (bar_iterator == bars.end()) {
      still_pending.push_back(id);
      continue;
    }
    const Bar& bar = *bar_iterator->second;

    double price = 0.0;
    bool triggered_now = false;
    if (!try_fill(order, bar, price, triggered_now)) {
      if (triggered_now) {
        order.triggered = true;
      }
      still_pending.push_back(id);
      continue;
    }
    if (triggered_now) {
      order.triggered = true;
    }

    double& available = liquidity[order.symbol];
    const double remaining = order.quantity - order.filled_quantity;
    const double quantity = std::min(remaining, available);
    if (quantity <= 1e-12) {
      still_pending.push_back(id);
      continue;
    }

    if (order.type == OrderType::Market || order.type == OrderType::Stop) {
      const double slippage = effective_slippage_bps(bar, quantity) / 10'000.0;
      price = order.side == Side::Buy ? price * (1.0 + slippage)
                                      : price * (1.0 - slippage);
      if (!std::isfinite(price) || price <= 0.0) {
        throw std::runtime_error("execution price is invalid");
      }
    }

    apply_fill(order, price, bar.timestamp, quantity);
    available -= quantity;
    if (order.status == OrderStatus::Pending ||
        order.status == OrderStatus::PartiallyFilled) {
      still_pending.push_back(id);
    }
  }

  pending_ = std::move(still_pending);
}

void Backtester::expire_day_orders() {
  for (auto iterator = pending_.begin(); iterator != pending_.end();) {
    auto order_iterator = orders_.find(*iterator);
    if (order_iterator != orders_.end() &&
        order_iterator->second.time_in_force == TimeInForce::Day &&
        order_iterator->second.expires_after_bar <= current_bar_) {
      order_iterator->second.status = OrderStatus::Expired;
      iterator = pending_.erase(iterator);
    } else {
      ++iterator;
    }
  }
}

Performance Backtester::run(const std::vector<Bar>& bars, Strategy& strategy) {
  if (bars.empty()) {
    throw std::invalid_argument("backtest requires at least one bar");
  }
  for (std::size_t index = 0; index < bars.size(); ++index) {
    detail::validate_bar(bars[index], "bar " + std::to_string(index));
  }

  reset();
  default_symbol_.clear();

  for (std::size_t index = 0; index < bars.size(); ++index) {
    current_bar_ = static_cast<std::int64_t>(index);
    if (index > 0) {
      execute_pending(bars[index]);
    }
    strategy.on_bar(bars[index], *this);
    expire_day_orders();
    marks_[default_symbol_] = bars[index].close;
    equity_curve_.push_back(equity(bars[index].close));
    equity_timestamps_.push_back(bars[index].timestamp);
  }

  return summarize(bars.size());
}

Performance Backtester::summarize(std::size_t bar_count) const {
  Performance performance;

  performance.bars = bar_count;
  performance.initial_equity = config_.initial_cash;
  performance.final_equity = equity_curve_.back();
  performance.total_return =
      performance.final_equity / performance.initial_equity - 1.0;
  performance.trades = fills_.size();
  performance.final_position = position();
  for (const auto& [symbol, position] : positions_) {
    performance.realized_pnl += position.realized_pnl;
    performance.winning_trades += position.winning_closes;
    performance.losing_trades += position.losing_closes;
    if (std::abs(position.quantity) > 1e-12) {
      performance.final_positions[symbol] = position.quantity;
    }
  }
  performance.total_commission = total_commission_;

  const std::size_t closed_trades =
      performance.winning_trades + performance.losing_trades;
  if (closed_trades > 0) {
    performance.win_rate = static_cast<double>(performance.winning_trades) /
                           static_cast<double>(closed_trades);
  }

  double equity_sum = 0.0;
  for (const double value : equity_curve_) {
    equity_sum += value;
  }
  const double average_equity =
      equity_sum / static_cast<double>(equity_curve_.size());
  if (average_equity > 0.0) {
    performance.turnover = total_notional_ / average_equity;
  }

  if (equity_curve_.size() > 1) {
    std::vector<double> returns;
    returns.reserve(equity_curve_.size() - 1);
    for (std::size_t index = 1; index < equity_curve_.size(); ++index) {
      if (equity_curve_[index - 1] != 0.0) {
        returns.push_back(equity_curve_[index] / equity_curve_[index - 1] -
                          1.0);
      }
    }

    if (!returns.empty()) {
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
      performance.annualized_volatility =
          deviation * std::sqrt(config_.periods_per_year);
      if (deviation > 0.0) {
        const double excess_mean =
            mean - config_.risk_free_rate / config_.periods_per_year;
        performance.sharpe_ratio =
            excess_mean / deviation * std::sqrt(config_.periods_per_year);
      }
      if (performance.initial_equity > 0.0 && performance.final_equity > 0.0) {
        const double periods = static_cast<double>(returns.size());
        performance.annualized_return =
            std::pow(performance.final_equity / performance.initial_equity,
                     config_.periods_per_year / periods) -
            1.0;
      }
    }
  }

  double peak = equity_curve_.front();
  for (const double value : equity_curve_) {
    peak = std::max(peak, value);
    if (peak > 0.0) {
      performance.max_drawdown =
          std::max(performance.max_drawdown, (peak - value) / peak);
    }
  }

  return performance;
}

SmaCrossoverStrategy::SmaCrossoverStrategy(int fast_window, int slow_window,
                                           double quantity, bool allow_short)
    : fast_window_(fast_window), slow_window_(slow_window), quantity_(quantity),
      allow_short_(allow_short) {
  if (fast_window < 1 || slow_window < 1) {
    throw std::invalid_argument("moving-average windows must be positive");
  }
  if (fast_window >= slow_window) {
    throw std::invalid_argument(
        "fast moving-average window must be shorter than the slow one");
  }
  if (!std::isfinite(quantity) || quantity <= 0.0) {
    throw std::invalid_argument("strategy quantity must be positive");
  }
}

void SmaCrossoverStrategy::on_bar(const Bar& bar, Backtester& engine) {
  closes_.push_back(bar.close);
  slow_sum_ += bar.close;

  if (static_cast<int>(closes_.size()) > slow_window_) {
    slow_sum_ -= closes_.front();
    closes_.pop_front();
  }
  if (static_cast<int>(closes_.size()) < slow_window_) {
    return;
  }

  const auto fast_begin = closes_.end() - fast_window_;
  const double fast_sum = std::accumulate(fast_begin, closes_.end(), 0.0);
  const double fast_average = fast_sum / static_cast<double>(fast_window_);
  const double slow_average = slow_sum_ / static_cast<double>(slow_window_);

  if (fast_average > slow_average) {
    engine.order_target(quantity_);
  } else if (fast_average < slow_average) {
    engine.order_target(allow_short_ ? -quantity_ : 0.0);
  }
}

BuyAndHoldStrategy::BuyAndHoldStrategy(double quantity) : quantity_(quantity) {
  if (!std::isfinite(quantity) || quantity <= 0.0) {
    throw std::invalid_argument("buy-and-hold quantity must be positive");
  }
}

void BuyAndHoldStrategy::on_bar(const Bar&, Backtester& engine) {
  if (!submitted_) {
    engine.buy(quantity_);
    submitted_ = true;
  }
}

} // namespace cppquant
