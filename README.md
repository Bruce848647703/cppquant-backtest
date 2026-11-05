# cppquant-backtest

A single-instrument event-driven backtester in modern C++20. Strategies submit
market orders, the engine fills them at the next bar's open with commission and
slippage, and the result is a full equity curve plus return, drawdown, Sharpe,
turnover and trade statistics.

[![CI](https://github.com/Bruce848647703/cppquant-backtest/actions/workflows/ci.yml/badge.svg)](https://github.com/Bruce848647703/cppquant-backtest/actions/workflows/ci.yml)

No third-party dependencies.

## Features

- **Event-driven loop**: orders submitted on bar `t` execute at bar `t+1`'s
  open, so strategies cannot look ahead.
- **Commission and slippage** in basis points, applied to both sides.
- **Long and short** positions with average-cost accounting and realized PnL.
- **Target positions**: `order_target(x)` accounts for pending orders and does
  not stack duplicate fills.
- **Metrics**: total and annualized return, annualized volatility, Sharpe
  ratio, maximum drawdown, turnover, win rate, commission paid.
- **CSV loader** (`timestamp,open,high,low,close,volume`) with validation.
- **Built-in strategies**: SMA crossover (long-only or long/short) and
  buy-and-hold.
- Tests, CLI, benchmark, CMake package export and multi-compiler CI.

## Quick start

Requirements: a C++20 compiler and CMake 3.20+. Ninja is optional.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Sanitizer build:

```bash
cmake -S . -B build-asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCPPQUANT_SANITIZE=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

Install and consume:

```bash
cmake --install build --prefix /tmp/cppquant
```

```cmake
find_package(cppquant_backtest CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE cppquant::backtest)
```

## Strategy API

```cpp
#include "cppquant/backtest.hpp"

using namespace cppquant;

class Momentum : public Strategy {
 public:
  void on_bar(const Bar& bar, Backtester& engine) override {
    if (bar.close > previous_close_ * 1.01) {
      engine.order_target(100.0);   // go long 100 units
    } else if (bar.close < previous_close_ * 0.99) {
      engine.close_position();
    }
    previous_close_ = bar.close;
  }

 private:
  double previous_close_{};
};

BacktestConfig config;
config.initial_cash = 100'000.0;
config.commission_bps = 1.0;
config.slippage_bps = 1.0;

Backtester engine(config);
Momentum strategy;
const Performance result = engine.run(bars, strategy);
```

Orders placed in `on_bar` are queued and filled at the **next** bar's open. The
last bar can leave pending orders; they are dropped because there is no next
open. `close_position()` is shorthand for `order_target(0)`.

## Command-line backtest

```bash
./build/backtest_cli --csv data/sample_bars.csv \
  --fast 5 --slow 20 --quantity 100 --fills
```

```text
cppquant-backtest
data: data/sample_bars.csv
bars: 300  (2020-01-01 .. 2020-10-26)
strategy: sma fast=5 slow=20 qty=100.0000 short=no

performance
  initial equity           100000.00
  final equity             100936.55
  total return                 0.94%
  annualized return            0.79%
  annualized vol               0.94%
  sharpe ratio                 0.843
  max drawdown                 1.15%
  turnover                     1.404
  trades                          13
  win rate                    33.33%
  commission paid              14.14
  realized pnl                824.64
  final position            100.0000

fills
  date         side        price     quantity commission
  2020-01-31   B         99.8620     100.0000       1.00
  2020-02-23   S        101.8384     100.0000       1.02
  ...
```

Useful flags: `--strategy buyhold`, `--short`, `--cash`, `--commission-bps`,
`--slippage-bps`, `--rf`, `--periods-per-year`, `--fills`, `--help`.

## CSV format

```text
timestamp,open,high,low,close,volume
1577836800,100.2089,101.4595,99.5818,101.0065,598262
1577923200,100.9140,101.0806,100.4939,100.6754,1162798
```

`timestamp` is an integer (epoch seconds in the sample). A header row is
optional; blank lines and `#` comments are ignored. Bars must have positive
prices, `high >= low`, `high >= open/close`, `low <= open/close` and
non-negative volume.

## Benchmark

```bash
./build/backtest_bench          # 1,000,000 synthetic bars
./build/backtest_bench --quick  # 100,000 bars, used by CI
```

Reference numbers from an Apple M-series laptop (Apple clang 21, `-O3`):

| Workload | Bars | Time | Throughput | Fills |
| --- | ---: | ---: | ---: | ---: |
| SMA(10,30) backtest | 1,000,000 | 32 ms | 30.8 M bars/s | 38,221 |

The engine is arithmetic-light per bar; the number mostly measures the strategy
callback and portfolio bookkeeping. Generation of the synthetic data is
excluded from the timing.

## Execution model

- Market orders only; there is no partial fill, queue position or latency
  model.
- Fill price is the next open, moved against the trader by
  `slippage_bps / 10,000`.
- Commission is `notional * commission_bps / 10,000`, charged on both buys and
  sells.
- Short selling is allowed and has no borrow cost or margin constraint; cash
  can go negative.
- Equity is `cash + position * close`. The final position is marked at the last
  close, not liquidated.
- `timestamp` is used for reporting only. Annualization uses
  `periods_per_year` (default 252), not the timestamp deltas.

## Design

```text
include/cppquant/backtest.hpp   public API
src/backtest.cpp                engine, accounting, metrics, strategies
src/csv.cpp                     CSV loader
src/validate.hpp                shared OHLC validation
examples/backtest_cli.cpp       CLI
```

At each bar the engine:

1. executes orders submitted on the previous bar at the current open;
2. calls `strategy.on_bar(bar, engine)`;
3. marks the portfolio at the current close and appends to the equity curve.

The position is tracked with average-cost accounting. Closing fills realize PnL
against the average cost; a fill that crosses through zero closes the old
position and opens the remainder at the fill price. Winning and losing closes
are counted separately for the win rate.

See [docs/DESIGN.md](docs/DESIGN.md) for the accounting rules, metric formulas
and test strategy.

## Limitations

- Single instrument and single strategy per run.
- Bar data only: no ticks, order book, limit orders or partial fills.
- No margin, borrow fees, corporate actions or transaction taxes.
- No walk-forward optimization or parameter sweeps; run the CLI per parameter
  set or drive `Backtester` from your own code.
- Metrics assume bars are evenly spaced and match `periods_per_year`.

Natural extensions are a multi-asset portfolio, limit/stop orders, a fill model
with participation limits, and a walk-forward experiment driver.

## Tests

The repository uses a small header-only test harness. The 15 tests cover:

- next-open execution, commission and slippage arithmetic;
- long, short, partial close and position-flip accounting;
- target orders, closing and end-of-data pending orders;
- equity curve, total return, drawdown, volatility, Sharpe and turnover;
- SMA crossover trading and short mode;
- CSV parsing, header handling and malformed input;
- invalid configuration and reset between runs.

```bash
./build/backtest_tests            # all tests
./build/backtest_tests csv        # filter by substring
```

## Roadmap

The 8-week plan (2026-11-09 to 2027-01-03), milestones and acceptance criteria
live in [docs/ROADMAP.md](docs/ROADMAP.md). Current and historical numbers are
in [docs/BENCHMARKS.md](docs/BENCHMARKS.md); user-visible changes are tracked in
[CHANGELOG.md](CHANGELOG.md).

## License

MIT. See [LICENSE](LICENSE).
