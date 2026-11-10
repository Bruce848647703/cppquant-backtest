# Walk-forward evaluation

## Windows

Walk-forward splits the bar series into rolling train/test windows:

```text
window 1: train [0, T)       test [T, T+S)
window 2: train [S, S+T)     test [T+S, T+2S)
window 3: train [2S, 2S+T)   test [T+2S, T+3S)
```

`T` is `train_bars`, `S` is `test_bars`, and the default step equals `S` so
test windows never overlap. A larger step leaves gaps; a smaller step is
rejected.

Training windows may overlap. That is normal: each window re-selects parameters
using only the bars before its test segment.

## Parameter selection

For each window, every `(fast, slow)` pair in the grid is backtested on the
training slice. The pair with the highest training total return is selected
(or Sharpe ratio when `select_by_sharpe` is true) and is then applied to the
test slice with a fresh strategy and a flat position.

The test slice is never used to choose parameters. The selected parameter is
recorded in `WalkForwardWindow::parameters`.

## Compounding

Test windows use the running out-of-sample equity as their initial cash. This
compounds returns across windows:

```text
equity_0 = initial_cash
for each window:
    run test with initial_cash = equity_i
    append the test equity curve
    equity_{i+1} = final test equity
```

Positions do not carry across window boundaries: each test starts flat with the
compounded cash. This avoids accidentally carrying a position through a period
that was never part of the training/test decision.

## Aggregate statistics

The stitched OOS curve is used to compute:

- final equity and total return;
- annualized return and volatility;
- Sharpe ratio with the configured risk-free rate;
- maximum drawdown;
- total fills.

Per-window training and test `Performance` values are available in
`WalkForwardWindow::training` and `WalkForwardWindow::testing`.

## API

```cpp
#include "cppquant/walk_forward.hpp"

const auto grid = make_sma_grid({5, 10}, {20, 30});

WalkForwardConfig config;
config.train_bars = 252;
config.test_bars = 63;
config.quantity = 100.0;
config.threads = 4;   // 0 = hardware concurrency, 1 = serial

const auto result = run_walk_forward(bars, grid, config);
for (const auto& window : result.windows) {
  // window.parameters, window.training, window.testing
}
// result.equity_curve, result.total_return, result.sharpe_ratio ...
```

## CLI

```bash
./build/walk_forward_cli --csv data/sample_bars.csv \
  --train 100 --test 50 \
  --fast 5,10 --slow 20,30 \
  --export walk_forward.csv
```

The CLI prints one row per window and the aggregate OOS statistics.

## CSV export

`write_walk_forward_csv` writes:

```text
window,train_start,train_end,test_start,test_end,fast,slow,
train_return,train_sharpe,test_return,test_sharpe,test_trades
```

Indices are half-open bar offsets into the input series.

## Validation and tests

- Every batch of bars is checked with the same OHLCV validator as the loader.
- `train_bars`, `test_bars` and the step must be positive; the step must be at
  least the test length.
- The grid must contain at least one valid `fast < slow` pair.
- Tests verify window ordering, non-overlapping test segments, short final
  windows, no-window behaviour, compounding, reproducibility, CSV rows and
  best-parameter selection on a trend.
- Parallel and serial runs are compared window by window; the worker count must
  not change any result.

## Limitations

- Selection is in-sample on the training window; there is no embargo or
  purged gap between train and test.
- Parameters are re-selected every window without a stability penalty.
- Positions are flat at each boundary; the engine does not model cross-window
  holding.
- Annualization uses `periods_per_year` from the backtest config, not the
  actual timestamp spacing.
- Only the built-in SMA strategy is wired into the walk-forward runner. A
  generic strategy factory is a natural extension.
- Training sweeps are parallelized, but windows remain sequential because cash
  compounds.
