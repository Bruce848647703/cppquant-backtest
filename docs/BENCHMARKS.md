# Benchmarks

Reference environment: Apple M-series (arm64), Apple clang 21.0.0,
`-O3 -DNDEBUG`, 2026-11-04.

| Workload | Bars | Time | Throughput | Fills |
| --- | ---: | ---: | ---: | ---: |
| SMA(10,30) backtest | 1,000,000 | 0.049 s | 20.3 M bars/s | 38,221 |

Command:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/backtest_bench
```

The run uses synthetic, deterministic data (seed 42); generation is excluded
from the timing. Numbers are indicative; run the benchmark on your own
hardware.

## W2: partial fills

The benchmark's 100-unit orders against roughly 1M volume stay below the
default 10% participation cap, so the W1 backtest numbers remain comparable.
The per-bar liquidity calculation adds a multiply, a minimum and a subtraction
per order. A dedicated partial-fill throughput baseline will be added with the
multi-symbol work.

## W3: multi-symbol panels

The panel layer is allocation-light and not separately benchmarked yet. The
single-symbol hot loop is unchanged; `run_batch` adds one string lookup per
batch for the trade symbol and passes the full batch to the strategy. A
multi-symbol throughput baseline will be added with the portfolio accounting
work.

## W4: multi-asset accounting

The 1M-bar single-symbol benchmark moved from about 32 ms to 49 ms (20.3M
bars/s) after the multi-asset refactor. Per-symbol position and mark lookups
replace direct scalar fields; the result is still comfortably above 20M
bars/s. Multi-asset batches add one hash lookup per pending order and a
per-symbol mark update per batch. A dedicated portfolio benchmark will be
added with the walk-forward/parallel sweep work.

## W5: walk-forward

Walk-forward cost is the number of windows times the grid size times the
training backtest, plus one test backtest per window. The sample run (300 bars,
100-bar train, 50-bar test, 4 parameter pairs) completes in well under a
second. A dedicated grid-size/window-count benchmark will be added with the
parallel sweep work.

## W6: parallel sweeps

The benchmark builds the 35 valid `fast < slow` pairs from
`{5,10,15,20,25,30} x {30,45,60,75,90,120}` and runs each pair over 1,000,000
bars. Every final equity is compared between runs.

| Threads | Pairs | Time | Throughput | Max equity difference |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 35 | 1.65 s | 21.2 pairs/s | — |
| 4 | 35 | 0.54 s | 65.1 pairs/s | 0.0 |

The four-thread run is **3.06x** faster, above the 2x acceptance target. The
speedup is sub-linear because each backtest allocates its own engine state and
the host has other work. `backtest_bench --quick` uses 100,000 bars and 16 pairs
for CI.

## W7: corporate actions and financing

Corporate actions are event-driven and do not touch the bar hot loop. When
financing is enabled, each bar performs one scan over open positions to accrue
margin and borrow costs; with a flat book this is a few integer comparisons.
The existing 1M-bar SMA benchmark is unchanged within run-to-run noise.

No separate throughput table is reported because the cost scales with the
number of open positions, not bars. The accounting identities and day-count
formulas are covered by unit tests.

## Planned baselines

- W8: final release numbers.
