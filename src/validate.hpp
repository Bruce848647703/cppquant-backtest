#pragma once

#include "cppquant/backtest.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

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

inline void validate_corporate_action(const CorporateAction& action,
                                      const std::string& context) {
  if (!std::isfinite(action.value)) {
    throw std::invalid_argument(context + ": value must be finite");
  }
  if (action.type == CorporateActionType::Split) {
    if (action.value <= 0.0) {
      throw std::invalid_argument(context + ": split ratio must be positive");
    }
  } else if (action.type == CorporateActionType::CashDividend) {
    if (action.value < 0.0) {
      throw std::invalid_argument(context + ": dividend must be non-negative");
    }
  } else {
    throw std::invalid_argument(context + ": unknown corporate action type");
  }
}

inline std::vector<CorporateAction>
prepare_corporate_actions(const std::vector<CorporateAction>& actions) {
  std::vector<CorporateAction> prepared = actions;
  for (std::size_t index = 0; index < prepared.size(); ++index) {
    validate_corporate_action(prepared[index],
                              "corporate action " + std::to_string(index));
  }
  std::stable_sort(
      prepared.begin(), prepared.end(),
      [](const CorporateAction& left, const CorporateAction& right) {
        return left.timestamp < right.timestamp;
      });
  return prepared;
}

} // namespace cppquant::detail
