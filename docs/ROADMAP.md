# cppquant-backtest roadmap

8-week plan: **2026-11-09 – 2027-01-03**. Goal: grow from a single-instrument
bar backtester into a multi-asset research framework with realistic orders and
walk-forward evaluation.

## Milestones

| Milestone | Date | Scope |
| --- | --- | --- |
| v0.2 | 2026-11-22 | limit/stop orders, partial fills |
| v0.3 | 2026-12-06 | multi-symbol data and portfolio accounting |
| v0.4 | 2026-12-20 | walk-forward, parallel sweeps |
| v0.5 | 2027-01-03 | corporate actions, costs, release |

## Weekly plan

### W1 (11-09 – 11-15): limit and stop orders

- Order state machine: pending, partially filled, filled, cancelled, expired.
- Limit fills from the next bar's OHLC with gap handling.
- Stop and stop-limit orders; DAY/GTC expiry.
- Document the intrabar ambiguity assumption.

Acceptance: a buy limit fills only when `low <= limit`, at
`min(open, limit)`; sell limit, stop, gap and expiry cases are tested; no
look-ahead.

### W2 (11-16 – 11-22): partial fills and participation

- Cap fills at a configurable share of bar volume.
- Keep unfilled quantity pending across bars; support cancellation.
- Slippage models: fixed bps, spread, simple impact.
- Zero-volume bars never fill.

Acceptance: cumulative fills never exceed order quantity; cash and position
stay correct after partial fills; cancelled orders never fill again.

### W3 (11-23 – 11-29): multi-symbol data

- `BarBatch` aligned by timestamp.
- `Strategy::on_bar(const BarBatch&, Backtester&)`, with a single-symbol
  adapter.
- Validate missing symbols, duplicate timestamps and ordering.
- Add a small multi-symbol sample.

Acceptance: alignment, missing data and ordering are tested; single-symbol
behaviour is unchanged; runs are reproducible.

### W4 (11-30 – 12-06): portfolio accounting

- Per-symbol position, average cost and realized PnL; shared cash.
- Target weights and rebalancing.
- Portfolio equity: `cash + sum(position * mark)`.
- Portfolio metrics.

Acceptance: cash conservation and equity identity are tested; a hand-computed
two-symbol rebalance matches; single-symbol metrics match the old engine.

### W5 (12-07 – 12-13): walk-forward

- Parameter grid and rolling train/test windows.
- Select parameters on the training window, apply only forward.
- Stitch out-of-sample equity curves.
- CLI flags for train/test length and grid values.
- Export per-window parameters and OOS metrics to CSV.

Acceptance: no leakage from test into selection; split boundaries and short
final windows are tested; results are reproducible.

### W6 (12-14 – 12-20): parallel sweeps

- Thread pool for independent parameter combinations.
- Deterministic result ordering.
- Benchmark serial versus four threads.
- Optional TSan test for a small sweep.

Acceptance: parallel results equal serial results; at least 2x speedup on four
cores for a sufficiently large sweep; no data races.

### W7 (12-21 – 12-27): corporate actions and financing

- Splits adjust position quantity and average cost.
- Cash dividends credit `position * dividend`.
- Margin and borrow costs accrue daily with configurable rates.
- Validate the event format.

Acceptance: equity is unchanged across a split apart from price adjustment;
dividends and financing costs match hand calculations.

### W8 (12-28 – 01-03): release and examples

- Add momentum, mean-reversion and multi-asset rebalancing examples.
- Document the order lifecycle, fill assumptions, walk-forward and costs.
- Tag `v0.5.0` and update `CHANGELOG.md`.

## Cut lines

1. Limit/stop orders and partial fills.
2. Multi-asset accounting and walk-forward.
3. Parallel sweeps, corporate actions and financing costs.

## Working agreement

- No backdated commits or empty changes.
- Every accounting change ships with an identity test.
- PRs merge only with green Linux CI and ASan/UBSan.
- Update `CHANGELOG.md` at every milestone.