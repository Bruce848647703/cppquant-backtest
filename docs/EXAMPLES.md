# Example strategies

`strategy_demo` contains three small, self-contained strategies that use only
the public API. They are examples, not investment advice.

## Momentum

A lookback return filter: go long when the close is at least `threshold` above
the close `lookback` bars ago, and flatten when it is at least `threshold`
below it.

```bash
./build/strategy_demo --bars data/sample_bars.csv \
  --strategy momentum --lookback 20 --threshold 0.02 --quantity 100
```

## Mean reversion

A rolling z-score filter: go long when the close is `z` standard deviations
below its rolling mean, and flatten when it is `z` standard deviations above.

```bash
./build/strategy_demo --bars data/sample_bars.csv \
  --strategy mean-reversion --lookback 20 --zscore 1.0 --quantity 100
```

## Equal-weight rebalance

A batch strategy that rebalances every N batches to equal notional weights
using `portfolio_equity()` and `order_target()`. Orders fill at the next batch,
so the example also demonstrates the next-open execution model.

```bash
./build/strategy_demo --panel data/sample_multi.csv \
  --strategy rebalance --rebalance-every 2
```

The bundled multi-symbol sample is intentionally tiny (four batches), so its
annualized statistics are not meaningful; use a longer panel for a realistic
run.

## Output

Each run prints the strategy configuration, final equity, total and annualized
return, volatility, Sharpe, drawdown, fills, commission and final positions.
The executable returns non-zero for invalid flags or malformed input.

## Tests

Two CTest smoke runs execute the momentum and rebalance examples against the
committed sample data. Both are also covered by the package-consumer build in
CI.