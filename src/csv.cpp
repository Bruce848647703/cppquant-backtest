#include "cppquant/backtest.hpp"

#include "validate.hpp"

#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cppquant {
namespace {

std::string trim(const std::string& value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

std::vector<std::string> split(const std::string& line) {
  std::vector<std::string> fields;
  std::string field;
  std::istringstream stream(line);
  while (std::getline(stream, field, ',')) {
    fields.push_back(trim(field));
  }
  return fields;
}

Bar parse_bar(const std::vector<std::string>& fields, std::size_t line_number) {
  if (fields.size() < 6) {
    throw std::runtime_error("line " + std::to_string(line_number) +
                             ": expected 6 columns");
  }

  Bar bar;
  try {
    bar.timestamp = std::stoll(fields[0]);
    bar.open = std::stod(fields[1]);
    bar.high = std::stod(fields[2]);
    bar.low = std::stod(fields[3]);
    bar.close = std::stod(fields[4]);
    bar.volume = std::stod(fields[5]);
  } catch (const std::invalid_argument&) {
    throw std::runtime_error("line " + std::to_string(line_number) +
                             ": invalid number");
  } catch (const std::out_of_range&) {
    throw std::runtime_error("line " + std::to_string(line_number) +
                             ": number out of range");
  }

  detail::validate_bar(bar, "line " + std::to_string(line_number));
  return bar;
}

Bar parse_symbol_bar(const std::vector<std::string>& fields,
                     std::size_t line_number) {
  if (fields.size() < 7) {
    throw std::runtime_error("line " + std::to_string(line_number) +
                             ": expected 7 columns");
  }

  Bar bar;
  try {
    bar.timestamp = std::stoll(fields[0]);
    bar.open = std::stod(fields[2]);
    bar.high = std::stod(fields[3]);
    bar.low = std::stod(fields[4]);
    bar.close = std::stod(fields[5]);
    bar.volume = std::stod(fields[6]);
  } catch (const std::invalid_argument&) {
    throw std::runtime_error("line " + std::to_string(line_number) +
                             ": invalid number");
  } catch (const std::out_of_range&) {
    throw std::runtime_error("line " + std::to_string(line_number) +
                             ": number out of range");
  }

  detail::validate_bar(bar, "line " + std::to_string(line_number));
  return bar;
}

} // namespace

std::vector<Bar> load_bars_csv(const std::string& path) {
  std::ifstream file(path);
  if (!file) {
    throw std::runtime_error("cannot open " + path);
  }

  std::vector<Bar> bars;
  std::string line;
  std::size_t line_number = 0;
  bool first_content_line = true;

  while (std::getline(file, line)) {
    ++line_number;
    const std::string cleaned = trim(line);
    if (cleaned.empty() || cleaned[0] == '#') {
      continue;
    }

    const auto fields = split(cleaned);
    if (first_content_line) {
      first_content_line = false;
      try {
        (void)std::stoll(fields.front());
      } catch (const std::exception&) {
        // A non-numeric first field is treated as a header row.
        continue;
      }
    }

    bars.push_back(parse_bar(fields, line_number));
  }

  if (bars.empty()) {
    throw std::runtime_error("no bars found in " + path);
  }
  return bars;
}

BarPanel load_bar_panel_csv(const std::string& path) {
  std::ifstream file(path);
  if (!file) {
    throw std::runtime_error("cannot open " + path);
  }

  std::map<std::string, std::vector<Bar>> grouped;
  std::string line;
  std::size_t line_number = 0;
  bool first_content_line = true;

  while (std::getline(file, line)) {
    ++line_number;
    const std::string cleaned = trim(line);
    if (cleaned.empty() || cleaned[0] == '#') {
      continue;
    }

    const auto fields = split(cleaned);
    if (first_content_line) {
      first_content_line = false;
      try {
        (void)std::stoll(fields.front());
      } catch (const std::exception&) {
        // A non-numeric first field is treated as a header row.
        continue;
      }
    }

    if (fields.size() < 7) {
      throw std::runtime_error("line " + std::to_string(line_number) +
                               ": expected 7 columns");
    }
    const std::string symbol = trim(fields[1]);
    if (symbol.empty()) {
      throw std::runtime_error("line " + std::to_string(line_number) +
                               ": empty symbol");
    }
    grouped[symbol].push_back(parse_symbol_bar(fields, line_number));
  }

  if (grouped.empty()) {
    throw std::runtime_error("no bars found in " + path);
  }

  std::vector<SymbolSeries> series;
  series.reserve(grouped.size());
  for (auto& [symbol, bars] : grouped) {
    series.push_back(SymbolSeries{std::move(symbol), std::move(bars)});
  }
  return BarPanel(std::move(series));
}

} // namespace cppquant
