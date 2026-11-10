# Benchmarks

Reference environment: Apple M-series (arm64), Apple clang 21.0.0,
`-O3 -DNDEBUG`, 2026-11-04.

| Workload | Bars | Time | Throughput | Fills |
| --- | ---: | ---: | ---: | ---: |
| SMA(10,30) backtest | 1,000,000 | 0.032 s | 30.8 M bars/s | 38,221 |

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

## Planned baselines

- W4: multi-symbol batch throughput and portfolio update cost.
- W5–W6: walk-forward and parallel sweep wall time versus serial.
- W7: corporate-action and financing-cost overhead.
