# Design notes

## Scope

`cppquant-backtest` is a deliberately small event-driven backtester for bar
data. It prioritizes deterministic accounting and a clear execution model over
feature breadth: no order book, no multi-asset portfolio yet, no parameter
optimization. W3 adds multi-symbol input and batch strategies while keeping
execution against one trade symbol; W4 adds multi-asset accounting.

## Event loop

```text
reset state
for i in 0 .. bars.size()-1:
    current_bar = i
    if i > 0:
        execute pending orders against bars[i] OHLC
    strategy.on_bar(bars[i], engine)
    expire DAY orders whose valid bar has passed
    equity_curve[i] = cash + position * bars[i].close
```

The strategy only sees the completed bar. Any order it submits becomes eligible
on the next bar, which avoids look-ahead bias with OHLC data. Orders left
pending after the last bar never execute.

## Orders and fills

The engine supports market, limit, stop and stop-limit orders, with DAY or GTC
time-in-force. Every order has a monotonically increasing id and a state:

```text
Pending -> PartiallyFilled -> Filled
   |              |
   +-> Cancelled  +-> Expired
   +-> Expired
```

W1 fills in full; W2 adds volume participation and partial fills.
`find_order`, `open_orders` and `cancel_order` expose the state machine to
strategies and tests.

### Bar liquidity and partial fills

Each bar has a shared fill budget:

```text
bar_liquidity = max(bar.volume, 0) * max_volume_participation
```

Pending orders are processed in submission order. Each order fills at most
`min(remaining, bar_liquidity)`, and the budget is reduced by the filled
quantity, so multiple orders cannot consume more than the configured share of
the bar. A zero-volume bar fills nothing.

An order with a positive filled quantity but remaining quantity becomes
`PartiallyFilled` and stays pending (unless its time-in-force expires). Its
average fill price and commission are accumulated across fills. Market orders
follow the same rule, so a large market order can execute over several bars;
use DAY to cancel the remainder at the end of its valid bar.

### Fill rules

All orders are evaluated against the next bar's OHLC:

| Order | Buy | Sell |
| --- | --- | --- |
| Market | `open` | `open` |
| Limit | `open <= limit`, else `low <= limit` | `open >= limit`, else `high >= limit` |
| Stop | `open >= stop`, else `high >= stop` | `open <= stop`, else `low <= stop` |
| Stop-limit | trigger as a stop, then behave as a limit | trigger as a stop, then behave as a limit |

Limit fills use the open when it is better than the limit; otherwise they use
the limit price. This models price improvement on gaps without assuming
favourable intrabar paths.

Stop-limit orders have two cases:

- A gap through the stop activates the limit at the open. If the open also
  satisfies the limit, the order fills immediately at the open.
- A trigger that only occurs inside the bar is **not** filled in the same bar.
  OHLC data cannot reveal whether the limit was reached before or after the
  stop, so the order becomes an active limit for the next bar. This is the
  conservative assumption.

### Slippage and commission

```text
market / stop fill = trigger * (1 + slippage)   for buys
                     trigger * (1 - slippage)   for sells
limit / stop-limit fill = open or limit price, never worse
commission = fill_price * quantity * commission_bps / 10,000
cash      -= signed_quantity * fill_price + commission
```

Slippage has three models:

| Model | Effective adverse move |
| --- | --- |
| `FixedBps` | `slippage_bps` |
| `Spread` | `spread_bps / 2` |
| `Impact` | `slippage_bps + impact_bps * sqrt(fill_qty / bar.volume)` |

Limit and stop-limit orders never pay slippage beyond their limit price.
Market and stop orders use the model above. Impact uses the quantity actually
filled in the bar, not the original order size.

`signed_quantity` is positive for buys and negative for sells. Commission is
always subtracted, so both sides pay it. A fill is recorded with its timestamp,
side, price, quantity, commission and order id.

### Time in force

- GTC remains pending until filled or cancelled.
- DAY is valid only for the next bar. If it is still pending at that bar's
  close, the remaining quantity expires. A DAY order submitted on the final bar
  has no valid bar and stays pending; callers should cancel it explicitly if
  that matters.

`order_target(target)` subtracts the current position **and any pending signed
quantity** from the target before submitting. Calling it every bar is therefore
idempotent once the target is reached; it does not stack duplicate orders.

## Multi-symbol panels

`BarPanel` takes one `SymbolSeries` per symbol and aligns them on the union of
timestamps. A `BarBatch` contains the symbols that have a bar at that
timestamp; missing bars are counted in total and per symbol.

Validation:

- symbols must be non-empty and unique;
- each series must have at least one bar;
- timestamps within a series must be strictly increasing (duplicates and
  unsorted rows are rejected);
- every OHLCV row is checked with the same validator as the single-symbol
  loader.

`BarPanel` preserves the input symbol order in each batch. The long-format CSV
loader groups rows by symbol, so the series order is lexicographic.

`Backtester::run_batch` validates that batches are non-empty, timestamps are
strictly increasing, symbols within a batch are unique, and the requested
`trade_symbol` is present in every batch. Orders execute against the trade
symbol's OHLC; the batch strategy can inspect all other symbols for signals.

`SingleSymbolBatchStrategy` adapts a single-symbol `Strategy` to one symbol in
the batch, which lets the existing strategy API run unchanged over a panel.

### Multi-asset accounting

Each symbol has its own position, average cost, realized PnL and winning/losing
close counts. Cash and commission are shared at the portfolio level.

```text
portfolio_equity = cash + sum(position[symbol] * mark[symbol])
```

Marks are updated from each batch's closes. If a held symbol is absent from a
batch, its last mark is retained; if no mark exists yet, average cost is used.
The no-symbol order overloads use `trade_symbol` as the default, while
`buy("AAA", qty)` and `order_target("BBB", qty)` target a specific symbol.

`Performance::final_positions` reports every non-zero final position.

## Position accounting

The engine uses average cost for each symbol's open position and realizes PnL
when a fill reduces or closes it.

Let `q` be the signed fill quantity, `p` the fill price, `P` the current
position and `A` its average cost.

**Opening or increasing** (`P == 0` or `sign(q) == sign(P)`):

```text
A_new = (A * P + p * q) / (P + q)
P_new = P + q
```

**Reducing, closing or flipping** (`sign(q) != sign(P)`):

```text
closed  = min(|P|, |q|)
pnl     = (p - A) * closed * sign(P)
P_new   = P + q
```

Realized PnL accumulates `pnl`. If the fill only reduces the position, the
average cost is unchanged. If it closes the position exactly, the average cost
resets to zero. If it flips the sign, the remainder opens at the fill price.

Each closing fill with positive PnL increments `winning_trades`; negative PnL
increments `losing_trades`. A single round trip can therefore produce more than
one "trade" in the win-rate statistic if it is closed in pieces. The `trades`
field counts fills, not round trips.

## Walk-forward

`run_walk_forward` rolls a training window of `train_bars` and a test window of
`test_bars`. The default step is the test length, so test segments never
overlap; a smaller step is rejected.

For every window:

1. slice `[train_start, train_end)` and `[test_start, test_end)`;
2. backtest every `(fast, slow)` SMA pair on the training slice;
3. select the best training total return (or Sharpe);
4. run the selected pair on the test slice with fresh state;
5. append the test equity curve and compound the cash.

The test slice is never used for selection. Positions are flat at each window
boundary, but cash carries forward, so the OOS curve compounds. The CSV export
contains one row per window with the ranges, parameters, training/test returns
and test fill count.

`WalkForwardResult` computes aggregate OOS return, volatility, Sharpe and
drawdown from the stitched equity curve.

## Performance metrics

Let `E_0 ... E_n` be the equity curve and `T` the number of return periods
(`n` when every previous equity is non-zero).

```text
total_return      = E_n / E_0 - 1
period_return[i]  = E_i / E_(i-1) - 1
mean              = average(period_return)
sd                = sample standard deviation of period_return
annualized_vol    = sd * sqrt(periods_per_year)
sharpe            = (mean - rf / periods_per_year) / sd * sqrt(periods_per_year)
annualized_return = (E_n / E_0)^(periods_per_year / T) - 1
```

`annualized_return` is a geometric extrapolation, not a prediction; it is
undefined for a non-positive final equity and is left at zero in that case.
`periods_per_year` defaults to 252 and is independent of the timestamps.

Maximum drawdown walks the equity curve:

```text
peak = max(E_0 ... E_i)
drawdown_i = (peak - E_i) / peak
max_drawdown = max(drawdown_i)
```

Turnover is total traded notional divided by the average equity:

```text
turnover = sum(|fill_price * fill_quantity|) / average(E_i)
```

The win rate is `winning_trades / (winning_trades + losing_trades)`, or zero
when there are no closes.

Annualization assumes evenly spaced bars. If bars are weekly or intraday, set
`periods_per_year` accordingly (52, 252, 252 * 390, ...). Timestamps are used
only for display.

## Validation

`Backtester::run` validates every bar before resetting state:

- all fields finite;
- prices positive;
- `high >= low`;
- `high >= open` and `high >= close`;
- `low <= open` and `low <= close`;
- volume non-negative.

The CSV loader performs the same validation and reports the line number.
Blank lines and `#` comments are ignored; the first non-comment line is skipped
as a header when its first field is not an integer.

Configuration is validated in the constructor: positive initial cash,
non-negative commission/slippage, positive `periods_per_year`, finite
risk-free rate. Order quantities must be finite and positive.

## Built-in strategies

### SMA crossover

Keeps up to `slow_window` closes in a deque and maintains the slow sum
incrementally. The fast sum is computed with `std::accumulate` over the last
`fast_window` values, which keeps the code simple for typical windows.

```text
fast_ma > slow_ma  -> target +quantity            (long)
fast_ma < slow_ma  -> target 0 or -quantity       (flat or short)
```

`order_target` makes repeated signals harmless. The strategy does not trade
while either moving average is warming up.

### Buy and hold

Submits a single market buy on the first bar and never trades again. Useful as
a benchmark and for accounting tests.

## Testing strategy

The tests use scripted strategies whose actions depend only on the bar index,
which makes every fill and cash movement predictable. They verify:

- next-open execution and the exact commission/slippage arithmetic;
- limit, stop and stop-limit fills on gaps and intrabar touches;
- DAY expiry, GTC persistence, cancellation and order-state queries;
- volume participation, shared bar liquidity, partial fills and zero-volume
  bars;
- fixed, spread and impact slippage models;
- multi-symbol union alignment, missing counts, long-format loading, batch
  execution and the single-symbol adapter;
- walk-forward window ordering, no-overlap validation, parameter selection,
  short final windows, compounding, reproducibility and CSV export;
- long, short, partial close and flip accounting;
- target orders, duplicate suppression and end-of-data pending orders;
- equity curve, total return, drawdown, volatility, Sharpe and turnover against
  hand-computed values;
- SMA crossover behaviour on constructed trends, including short mode;
- CSV parsing, header detection, malformed rows and OHLC validation;
- configuration and order validation;
- state reset when the same engine is reused.

CI runs the suite with GCC, Clang and Apple clang, plus ASan/UBSan. The
benchmark runs in `--quick` mode as a smoke test.

## Possible extensions

- A portfolio of instruments with per-symbol positions and marks.
- Queue position, latency and tick-level fills.
- Discrete dividends, splits and borrow costs.
- Walk-forward parameter sweeps with out-of-sample reporting.
- An equity-curve export (CSV/JSON) for plotting.
- A reference vector implementation for property-based tests of accounting.
