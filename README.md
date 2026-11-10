# cppquant-backtest

A single-instrument event-driven backtester in modern C++20. Strategies submit
market orders, the engine fills them at the next bar's open with commission and
slippage, and the result is a full equity curve plus return, drawdown, Sharpe,
turnover and trade statistics.

[![CI](https://github.com/Bruce848647703/cppquant-backtest/actions/workflows/ci.yml/badge.svg)](https://github.com/Bruce848647703/cppquant-backtest/actions/workflows/ci.yml)

No third-party dependencies.

## Features

- **Event-driven loop**: orders submitted on bar `t` become eligible on bar
  `t+1`, so strategies cannot look ahead.
- **Order types**: market, limit, stop and stop-limit, with DAY and GTC
  time-in-force and an explicit order state machine.
- **Commission and slippage** in basis points; limit fills never trade worse
  than the limit price.
- **Partial fills**: per-bar volume participation is shared across orders and
  unfilled quantity stays pending across bars.
- **Slippage models**: fixed bps, half-spread and square-root volume impact.
- **Long and short** positions with average-cost accounting and realized PnL.
- **Order queries**: `find_order`, `open_orders` and `cancel_order`.
- **Target positions**: `order_target(x)` accounts for pending orders and does
  not stack duplicate fills.
- **Metrics**: total and annualized return, annualized volatility, Sharpe
  ratio, maximum drawdown, turnover, win rate, commission paid.
- **Single-symbol CSV loader** (`timestamp,open,high,low,close,volume`) with
  validation.
- **Multi-symbol panels**: union-aligned `BarBatch` data, missing-bar counts,
  a batch strategy API and a long-format CSV loader.
- **Multi-asset portfolio**: per-symbol positions, average cost, realized PnL,
  shared cash, target quantities and portfolio equity.
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

### Order types

```cpp
engine.buy(100.0);                                  // market
engine.buy_limit(100.0, 95.0);                      // buy limit
engine.sell_stop(100.0, 90.0);                       // sell stop
engine.buy_stop_limit(100.0, 105.0, 106.0);          // buy stop-limit
engine.buy_limit(100.0, 95.0, TimeInForce::Day);     // expires after next bar

OrderId id = engine.buy_limit(100.0, 95.0);
if (const OrderState* order = engine.find_order(id)) {
  // order->status, order->filled_quantity, order->average_fill_price ...
}
engine.cancel_order(id);
```

Fill rules use the next bar's OHLC:

| Order | Buy | Sell |
| --- | --- | --- |
| Market | next open | next open |
| Limit | `open <= limit`, else `low <= limit` | `open >= limit`, else `high >= limit` |
| Stop | `open >= stop`, else `high >= stop` | `open <= stop`, else `low <= stop` |
| Stop-limit | trigger as a stop, then behave as a limit | trigger as a stop, then behave as a limit |

Limit fills use the open when it is better than the limit, otherwise the limit
price. A stop-limit that is triggered only inside a bar waits for the next bar:
OHLC data cannot reveal whether the limit was reached before or after the stop.
A gap through the stop activates the limit immediately.

`OrderStatus` is one of `Pending`, `PartiallyFilled`, `Filled`, `Cancelled` or
`Expired`. W1 fills orders in full; partial fills arrive with the volume
participation work in W2.

## Multi-symbol panels

`BarPanel` aligns per-symbol series on the union of timestamps. Each `BarBatch`
contains the symbols that have a bar at that timestamp; missing bars are counted
per symbol.

```cpp
#include "cppquant/backtest.hpp"

const BarPanel panel = load_bar_panel_csv("data/sample_multi.csv");

class Signal : public BatchStrategy {
 public:
  void on_batch(const BarBatch& batch, Backtester& engine) override {
    const Bar* a = batch.find("AAA");
    const Bar* b = batch.find("BBB");
    if (a && b) {
      engine.order_target(b->close > a->close ? 10.0 : 0.0);
    }
  }
};

Signal strategy;
Backtester engine;
const Performance result =
    engine.run_batch(panel.batches(), "AAA", strategy);
```

Orders can target any symbol in the batch, for example
`engine.buy("AAA", 10.0)` or `engine.order_target("BBB", 5.0)`. Cash is shared
across the portfolio, while position, average cost and realized PnL are tracked
per symbol. The overloads without a symbol use `trade_symbol` as the default.

`engine.portfolio_equity()` marks every position at its last known close and
falls back to average cost when a symbol has no mark yet. Target weights can be
implemented by sizing each `order_target` from portfolio equity and the current
price.

```text
cppquant-backtest panel
symbols: 2, batches: 4, missing bars: 1
final equity: 100059.59
trades: 2
```

```bash
./build/panel_backtest data/sample_multi.csv 10
```

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
| SMA(10,30) backtest | 1,000,000 | 49 ms | 20.3 M bars/s | 38,221 |

The engine is arithmetic-light per bar; the number mostly measures the strategy
callback and portfolio bookkeeping. Generation of the synthetic data is
excluded from the timing.

## Execution model

- Orders are submitted from `Strategy::on_bar` and matched against the next
  bar's OHLC.
- A bar can fill at most `volume * max_volume_participation` across all
  pending orders (default 10%). Unfilled quantity stays pending for GTC orders
  or expires for DAY orders.
- Market and stop fills pay the configured slippage; limit and stop-limit fills
  never trade worse than the limit price. Market orders can therefore fill in
  pieces across several bars.
- Slippage models:
  - `FixedBps` uses `slippage_bps`.
  - `Spread` uses half of `spread_bps`.
  - `Impact` uses `slippage_bps + impact_bps * sqrt(fill_qty / bar.volume)`.
- Commission is `notional * commission_bps / 10,000`, charged on every fill.
- DAY orders are valid for the next bar only; unfilled quantity expires at that
  bar's close. GTC orders remain until filled or cancelled.
- Short selling is allowed and has no borrow cost or margin constraint; cash
  can go negative.
- Equity is `cash + position * close`. The final position is marked at the last
  close, not liquidated.
- `timestamp` is used for reporting only. Annualization uses
  `periods_per_year` (default 252), not the timestamp deltas.

```cpp
BacktestConfig config;
config.max_volume_participation = 0.10;
config.slippage_model = SlippageModel::Impact;
config.impact_bps = 20.0;
```

## Design

```text
include/cppquant/backtest.hpp   public API
src/backtest.cpp                engine, accounting, metrics, strategies
src/csv.cpp                     single and long-format CSV loaders
src/panel.cpp                   multi-symbol alignment and batch loop
src/validate.hpp                shared OHLC validation
examples/backtest_cli.cpp       single-symbol CLI
examples/panel_backtest.cpp     multi-symbol CLI
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

- Bar data only: no ticks, order book, queue position or latency model.
- No short-borrow costs, margin calls or per-symbol risk limits yet.
- No margin, borrow fees, corporate actions or transaction taxes.
- No walk-forward optimization or parameter sweeps; run the CLI per parameter
  set or drive `Backtester` from your own code.
- Metrics assume bars are evenly spaced and match `periods_per_year`.

Natural extensions are walk-forward evaluation, a multi-asset risk model and a
fill model with queue position and latency.

## Tests

The repository uses a small header-only test harness. The 51 tests cover:

- next-open execution, commission and slippage arithmetic;
- market, limit, stop and stop-limit fills, gaps and intrabar touch rules;
- DAY expiry, GTC persistence, cancellation and order-state queries;
- volume participation, shared bar liquidity, partial fills across bars and
  zero-volume bars;
- multi-symbol union alignment, missing-bar counts, long-format CSV loading,
  batch execution and the single-symbol adapter;
- multi-asset positions, shared cash, per-symbol realized PnL, target weights,
  last-mark equity and per-symbol close;
- fixed, spread and impact slippage models;
- long, short, partial close and position-flip accounting;
- target orders, closing and end-of-data pending orders;
- equity curve, total return, drawdown, volatility, Sharpe and turnover;
- SMA crossover trading and short mode;
- CSV parsing, header handling and malformed input;
- invalid configuration, invalid orders and reset between runs.

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
