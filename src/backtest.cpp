#include "cppquant/backtest.hpp"

#include "validate.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

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
  position_ = 0.0;
  average_cost_ = 0.0;
  realized_pnl_ = 0.0;
  total_commission_ = 0.0;
  total_notional_ = 0.0;
  next_order_id_ = 1;
  winning_closes_ = 0;
  losing_closes_ = 0;
  pending_.clear();
  fills_.clear();
  equity_curve_.clear();
  equity_timestamps_.clear();
}

void Backtester::buy(double quantity) {
  if (!std::isfinite(quantity) || quantity <= 0.0) {
    throw std::invalid_argument("buy quantity must be positive and finite");
  }
  pending_.push_back(PendingOrder{next_order_id_++, Side::Buy, quantity});
}

void Backtester::sell(double quantity) {
  if (!std::isfinite(quantity) || quantity <= 0.0) {
    throw std::invalid_argument("sell quantity must be positive and finite");
  }
  pending_.push_back(PendingOrder{next_order_id_++, Side::Sell, quantity});
}

void Backtester::order_target(double target_position) {
  if (!std::isfinite(target_position)) {
    throw std::invalid_argument("target position must be finite");
  }

  double pending_quantity = 0.0;
  for (const auto& order : pending_) {
    pending_quantity +=
        order.side == Side::Buy ? order.quantity : -order.quantity;
  }

  const double delta = target_position - (position_ + pending_quantity);
  if (delta > 1e-12) {
    buy(delta);
  } else if (delta < -1e-12) {
    sell(-delta);
  }
}

void Backtester::close_position() {
  order_target(0.0);
}

void Backtester::execute_pending(double open_price, std::int64_t timestamp) {
  const double slippage = config_.slippage_bps / 10'000.0;

  for (const auto& order : pending_) {
    const double price = order.side == Side::Buy
                             ? open_price * (1.0 + slippage)
                             : open_price * (1.0 - slippage);
    if (!std::isfinite(price) || price <= 0.0) {
      throw std::runtime_error("execution price is invalid");
    }
    apply_fill(order, price, timestamp);
  }
  pending_.clear();
}

void Backtester::apply_fill(const PendingOrder& order, double price,
                            std::int64_t timestamp) {
  const double signed_quantity =
      order.side == Side::Buy ? order.quantity : -order.quantity;
  const double notional = price * order.quantity;
  const double commission = notional * config_.commission_bps / 10'000.0;

  cash_ -= signed_quantity * price + commission;
  total_commission_ += commission;
  total_notional_ += notional;

  const double new_position = position_ + signed_quantity;

  if (std::abs(position_) < 1e-12) {
    average_cost_ = price;
  } else if ((position_ > 0.0) == (signed_quantity > 0.0)) {
    average_cost_ =
        (average_cost_ * position_ + price * signed_quantity) / new_position;
  } else {
    const double closing =
        std::min(std::abs(position_), std::abs(signed_quantity));
    const double direction = position_ > 0.0 ? 1.0 : -1.0;
    const double pnl = (price - average_cost_) * closing * direction;
    realized_pnl_ += pnl;
    if (pnl > 0.0) {
      ++winning_closes_;
    } else if (pnl < 0.0) {
      ++losing_closes_;
    }

    if (std::abs(new_position) < 1e-12) {
      average_cost_ = 0.0;
    } else if ((new_position > 0.0) != (position_ > 0.0)) {
      average_cost_ = price;
    }
  }

  position_ = new_position;
  if (std::abs(position_) < 1e-12) {
    position_ = 0.0;
    average_cost_ = 0.0;
  }

  fills_.push_back(
      Fill{order.id, timestamp, order.side, price, order.quantity, commission});
}

Performance Backtester::run(const std::vector<Bar>& bars, Strategy& strategy) {
  if (bars.empty()) {
    throw std::invalid_argument("backtest requires at least one bar");
  }
  for (std::size_t index = 0; index < bars.size(); ++index) {
    detail::validate_bar(bars[index], "bar " + std::to_string(index));
  }

  reset();

  for (std::size_t index = 0; index < bars.size(); ++index) {
    if (index > 0) {
      execute_pending(bars[index].open, bars[index].timestamp);
    }
    strategy.on_bar(bars[index], *this);
    equity_curve_.push_back(equity(bars[index].close));
    equity_timestamps_.push_back(bars[index].timestamp);
  }

  Performance performance;
  performance.bars = bars.size();
  performance.initial_equity = config_.initial_cash;
  performance.final_equity = equity_curve_.back();
  performance.total_return =
      performance.final_equity / performance.initial_equity - 1.0;
  performance.trades = fills_.size();
  performance.winning_trades = winning_closes_;
  performance.losing_trades = losing_closes_;
  performance.total_commission = total_commission_;
  performance.realized_pnl = realized_pnl_;
  performance.final_position = position_;

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
