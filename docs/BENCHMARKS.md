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

## Planned baselines

- W6: parallel sweep wall time versus serial.
- W7: corporate-action and financing-cost overhead.
