#pragma once

#include "cppquant/backtest.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace cppquant::detail {

inline void validate_bar(const Bar& bar, const std::string& context) {
  if (!std::isfinite(bar.open) || !std::isfinite(bar.high) ||
      !std::isfinite(bar.low) || !std::isfinite(bar.close) ||
      !std::isfinite(bar.volume)) {
    throw std::invalid_argument(context + ": bar values must be finite");
  }
  if (bar.open <= 0.0 || bar.high <= 0.0 || bar.low <= 0.0 ||
      bar.close <= 0.0) {
    throw std::invalid_argument(context + ": prices must be positive");
  }
  if (bar.high < bar.low) {
    throw std::invalid_argument(context + ": high is below low");
  }
  if (bar.high < std::max(bar.open, bar.close) ||
      bar.low > std::min(bar.open, bar.close)) {
    throw std::invalid_argument(context + ": OHLC values are inconsistent");
  }
  if (bar.volume < 0.0) {
    throw std::invalid_argument(context + ": volume must be non-negative");
  }
}

} // namespace cppquant::detail
