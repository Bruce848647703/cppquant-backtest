# Corporate actions and financing

## Event format

`CorporateAction` carries one effective timestamp, an optional symbol, a type
and a value:

```text
timestamp,symbol,type,value
1580515200,SAMPLE,split,2
1583020800,SAMPLE,dividend,0.5
```

`type` is `split` or `dividend`/`cash_dividend`. A split value is the ratio of
new shares to old shares; a dividend value is cash per share. The loader skips
a header and `#` comments, rejects unknown types and non-finite or negative
values, and returns actions sorted by timestamp (stable for ties).

```cpp
const auto actions = load_corporate_actions_csv("data/sample_corporate_actions.csv");
const Performance result = engine.run(bars, strategy, actions);
```

Actions are applied at the first bar or batch whose timestamp is at or after
the action timestamp, before pending orders execute. A single-symbol run
ignores the symbol field. A batch run requires a non-empty symbol that appears
in the input.

## Splits

For a ratio `r`:

```text
position quantity *= r
average cost      /= r
last mark         /= r
```

Pending and partially-filled order quantities and fill quantities also scale by
`r`, while limit and stop prices divide by `r`. Cash is unchanged. Equity is
therefore unchanged across the split once the data uses post-split prices.

## Cash dividends

A dividend credits `position * dividend` to cash on the effective bar. Short
positions are debited. The contribution is reported as
`Performance::total_dividends` and can be negative.

Dividends do not adjust average cost or historical fills. This is an ex-date
cash model, not a separate payable date.

## Margin and borrow costs

`BacktestConfig::margin_rate` is charged on negative cash and
`borrow_rate` on the market value of short positions. Both are annual rates
with an actual/365 day count based on bar timestamps:

```text
margin_cost = max(-cash, 0) * margin_rate * days / 365
borrow_cost = sum_short(-quantity * last_mark) * borrow_rate * days / 365
```

Costs are accrued at the start of a bar using the previous bar's marks,
before corporate actions and pending executions. The first bar has no accrual.
Totals are reported as `total_margin_cost` and `total_borrow_cost`; negative
rates act as credits.

The benchmark assumes bar timestamps are epoch seconds. Intraday bars accrue a
fraction of a day.

## CLI

```bash
./build/backtest_cli --csv data/sample_bars.csv --strategy buyhold \
  --actions data/sample_corporate_actions.csv \
  --margin-rate 0.02 --borrow-rate 0.01
```

The performance block prints dividends, margin cost and borrow cost.

## Validation and tests

- Split changes position quantity and average cost while keeping equity
  invariant against a pre-split run.
- A pending limit order is rescaled and fills at the adjusted price.
- Long dividends credit cash; short dividends debit it.
- Margin cost matches `negative_cash * rate / 365` for one day.
- Borrow cost matches `short_value * rate / 365`.
- Batch splits apply only to the matching symbol.
- Empty or unknown batch action symbols are rejected.
- CSV loading handles headers, comments, sorting and malformed rows.

## Limitations

- No payable date, withholding tax or special-dividend flag.
- No merger, spin-off, rights issue or symbol change.
- Splits adjust open orders, but historical fills are not restated.
- Financing uses actual/365 and bar timestamps; there is no exchange calendar
  or day-count convention selection.
- Margin calls and buying-power constraints are not modelled.