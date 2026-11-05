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

## Planned baselines

- W2: limit/stop order throughput and partial-fill overhead.
- W3–W4: multi-symbol batch throughput and portfolio update cost.
- W5–W6: walk-forward and parallel sweep wall time versus serial.
- W7: corporate-action and financing-cost overhead.
