#include "cppquant/backtest.hpp"
#include "test_framework.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

using namespace cppquant;

namespace {

std::filesystem::path temporary_path(const std::string& name) {
  return std::filesystem::temp_directory_path() / name;
}

void write_file(const std::filesystem::path& path, const std::string& text) {
  std::ofstream file(path);
  file << text;
}

} // namespace

TEST(csv_loader_rejects_wrong_column_counts) {
  const auto bars_path = temporary_path("cppquant_csv_columns.csv");
  write_file(bars_path, "0,100,101\n");
  CHECK_THROW(load_bars_csv(bars_path.string()), std::runtime_error);
  std::remove(bars_path.string().c_str());

  const auto panel_path = temporary_path("cppquant_panel_columns.csv");
  write_file(panel_path, "0,AAA,100,101,99,100\n");
  CHECK_THROW(load_bar_panel_csv(panel_path.string()), std::runtime_error);
  std::remove(panel_path.string().c_str());
}

TEST(csv_loader_rejects_invalid_numbers) {
  const auto bars_path = temporary_path("cppquant_csv_numbers.csv");
  write_file(bars_path, "0,100,101,99,100,1000\n"
                        "86400,100,not-a-number,99,100,1000\n");
  CHECK_THROW(load_bars_csv(bars_path.string()), std::runtime_error);
  std::remove(bars_path.string().c_str());

  const auto panel_path = temporary_path("cppquant_panel_numbers.csv");
  write_file(panel_path, "0,AAA,100,101,99,100,1000\n"
                         "86400,AAA,100,101,99,100,not-a-number\n");
  CHECK_THROW(load_bar_panel_csv(panel_path.string()), std::runtime_error);
  std::remove(panel_path.string().c_str());
}

TEST(csv_loader_rejects_out_of_range_numbers) {
  const auto bars_path = temporary_path("cppquant_csv_range.csv");
  write_file(bars_path, "0,100,101,99,100,1000\n"
                        "999999999999999999999,100,101,99,100,1000\n");
  CHECK_THROW(load_bars_csv(bars_path.string()), std::runtime_error);
  std::remove(bars_path.string().c_str());

  const auto panel_path = temporary_path("cppquant_panel_range.csv");
  write_file(panel_path, "0,AAA,100,101,99,100,1000\n"
                         "999999999999999999999,AAA,100,101,99,100,1000\n");
  CHECK_THROW(load_bar_panel_csv(panel_path.string()), std::runtime_error);
  std::remove(panel_path.string().c_str());
}

TEST(csv_loader_rejects_empty_symbols_and_empty_files) {
  const auto panel_path = temporary_path("cppquant_panel_symbol.csv");
  write_file(panel_path, "0,AAA,100,101,99,100,1000\n"
                         "86400,,100,101,99,100,1000\n");
  CHECK_THROW(load_bar_panel_csv(panel_path.string()), std::runtime_error);
  std::remove(panel_path.string().c_str());

  const auto bars_path = temporary_path("cppquant_csv_empty.csv");
  write_file(bars_path, "timestamp,open,high,low,close,volume\n");
  CHECK_THROW(load_bars_csv(bars_path.string()), std::runtime_error);
  std::remove(bars_path.string().c_str());

  const auto empty_panel = temporary_path("cppquant_panel_empty.csv");
  write_file(empty_panel, "# no rows\n");
  CHECK_THROW(load_bar_panel_csv(empty_panel.string()), std::runtime_error);
  std::remove(empty_panel.string().c_str());
}