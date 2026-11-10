# Parallel parameter sweeps

## API

`run_sma_sweep` backtests every `(fast, slow)` pair independently and returns
results in the same order as the input grid:

```cpp
#include "cppquant/sweep.hpp"

const auto grid = make_sma_grid({5, 10, 20}, {30, 50, 80});

SmaSweepConfig config;
config.backtest.initial_cash = 100'000.0;
config.quantity = 100.0;
config.threads = 4;              // 0 = hardware concurrency, 1 = serial

const auto results = run_sma_sweep(bars, grid, config);
for (const auto& entry : results) {
  // entry.parameters, entry.performance
}
```

`SmaSweepResult` values are stored by grid index, not completion order. Serial
and parallel runs therefore produce identical performance numbers and ties
select the earlier grid entry.

## Worker model

`run_sma_sweep` starts `min(resolved_threads, grid.size())` workers. Each worker
pulls the next grid index from an atomic counter and constructs its own
`Backtester` and `SmaCrossoverStrategy`. The bar vector and configuration are
read-only, so no locks are needed in the hot path. Only the first worker
exception is captured, then rethrown on the calling thread after all workers
join.

`threads = 0` uses `std::thread::hardware_concurrency()`, with a fallback of
one. `resolve_thread_count` exposes the same policy.

## Walk-forward integration

`WalkForwardConfig::threads` is forwarded to the training sweep of every
window. Cash still compounds sequentially across windows: only the independent
parameter combinations inside one training window run in parallel.

The selected parameters and all out-of-sample results are independent of the
worker count.

## Benchmark

`backtest_bench` runs the same 35-pair, 1,000,000-bar sweep with one and four
threads and compares every final equity:

```text
sweep 35 pairs, 1 threads  1.649 s  21.2 pairs/s
sweep 35 pairs, 4 threads  0.538 s  65.1 pairs/s
serial/parallel max equity difference: 0.000e+00  speedup: 3.06x
```

The four-thread result is above the 2x acceptance target. The speedup is
sub-linear because each backtest still allocates its own engine state and the
machine has other work.

## Validation

- Serial and parallel sweeps are compared field by field for every grid entry.
- Each sweep result matches a direct `Backtester::run` call.
- Parallel and serial walk-forward runs produce identical windows, parameters,
  equity curves and aggregate statistics.
- Invalid bars, grids, quantities and backtest settings are rejected, including
  exceptions raised inside a worker.
- TSan runs the parallel sweep tests in CI.

## Limitations

- Sweeps are CPU-only and recreate a worker group per call; there is no
  persistent global thread pool.
- Only the built-in SMA crossover strategy is swept.
- Results are held in memory; the API does not stream to disk.