# Changelog

All notable changes to this project are documented here.

## [Unreleased]

### Added

- Limit, stop and stop-limit orders with next-bar OHLC fill rules and gap
  handling.
- Order state machine (`Pending`, `PartiallyFilled`, `Filled`, `Cancelled`,
  `Expired`) with `find_order`, `open_orders` and `cancel_order`.
- DAY and GTC time-in-force; DAY orders expire after their valid bar.
- Conservative stop-limit semantics: an intrabar trigger waits for the next
  bar, while a gap through the stop activates the limit immediately.
- Tests for limit/stop fills, gaps, expiry, cancellation, state queries and
  invalid orders.
- Volume participation caps shared across all orders in a bar; zero-volume bars
  never fill.
- Partial fills across bars with `PartiallyFilled` order state, average fill
  price and per-fill commissions.
- Fixed-bps, half-spread and square-root impact slippage models.
- Tests for participation, aggregate liquidity, day expiry after partial
  fills, partial cancellation and slippage models.

### Planned

- **0.2** — limit/stop orders, partial fills, volume participation.
- **0.3** — multi-symbol data, portfolio accounting and target weights.
- **0.4** — walk-forward evaluation and parallel parameter sweeps.
- **0.5** — corporate actions, financing costs, examples and release.

## [0.1.0] - 2026-11-04

### Added

- Event-driven, single-instrument bar backtester.
- Next-open execution with commission and slippage in basis points.
- Average-cost accounting for long, short, partial close and position flips.
- Target positions that account for pending orders.
- Equity curve, return, volatility, Sharpe, drawdown, turnover and win rate.
- Validated CSV loader, SMA crossover and buy-and-hold strategies.
- CLI, deterministic benchmark, tests and documentation.
