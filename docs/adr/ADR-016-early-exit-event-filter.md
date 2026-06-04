# ADR-016 — Early-Exit Event Filter: Discard Irrelevant or Anomalous Messages Before the Pipeline

## Status
Proposed — Deferred

## Date
2026-06-04

## Context

The current pipeline processes every message that passes `arbitrate` without inspecting whether the message is relevant to the tracked instrument set or whether its fields are within plausible market bounds. Two classes of problem motivate an explicit filter stage:

**Irrelevant messages:**
A raw NASDAQ feed carries ITCH messages for all 8,000+ symbols. Even after ADR-014's per-instrument routing table is in place, a stock_locate value that does not map to any tracked pipeline slot is currently undefined behaviour — the routing logic receives a message for an instrument it has no state for. A filter stage that drops unrecognised stock_locate values before the routing lookup closes this hole cleanly.

**Anomalous messages:**
Market data occasionally contains messages that are technically valid ITCH but operationally meaningless: prices thousands of ticks away from the current market (fat-finger entries, clearly erroneous quotes), quantities of 1 share on a multi-lot-size market (noise orders placed to probe the book), or prices that would compute to a negative index under the `base_price / tick_size` formula. Passing these through the full pipeline corrupts book state — a single erroneous price 100,000 ticks from base would write to an out-of-range BRAM address or produce an obviously wrong snapshot.

A filter stage inserted between `arbitrate` and `parse_add_event` provides an early-exit point: consume from the input stream, apply configurable checks, conditionally forward to the downstream stages. Dropped messages do not proceed. The downstream stages (`parse_add_event`, `store_buffer_write`, `update_snapshot`) see only valid, relevant messages — their logic remains clean.

## Decision

Add a `filter_event` DATAFLOW task between `arbitrate` and `parse_add_event`.

**Position in pipeline:**

```
arbitrate → filter_event → parse_add_event → store_buffer_write → update_snapshot
```

`filter_event` reads an `ap_uint<288>` ITCH payload from the `arbitrate` output stream. It applies three checks in order of cheapness (cheapest first, short-circuit on first failure):

**Check 1 — Instrument membership (1 cycle):**
Extract the 16-bit `stock_locate` field from the payload. Look up in the ADR-014 LUTRAM routing table. If no entry exists (slot index == INVALID_SLOT), drop the message and increment `drop_count_membership`. This reuses the routing table infrastructure from ADR-014 — no additional memory required.

**Check 2 — Price sanity (combinatorial):**
Extract the raw price field (32-bit, ITCH fixed-point). Two sub-checks:

- **Upper bound (hard drop):** if `price - base_price > max_ticks` (configurable via `s_axilite`, default: 2,000 ticks above base), drop the message. An index beyond `BOOK_DEPTH` would write to an out-of-range BRAM address and corrupt adjacent instrument state. This is the only case where a drop is unambiguously correct. A static global default is inappropriate for volatile instruments — a small-cap moving 20% intraday legitimately exceeds 2,000 ticks and would have valid orders hard-dropped. The ML batch job must calibrate `max_ticks_above_base` per instrument slot from historical intraday range (e.g., ATR × safety multiplier), same pattern as `min_qty`. A miscalibrated threshold silently degrades book quality without triggering any error.

- **Lower bound (configuration anomaly, not a hard drop):** if `price < base_price`, the index computation underflows. This is not necessarily a bad market message — it may indicate that `base_price` is stale (the market has moved down significantly since session open). Dropping it silently loses a potentially valid order. Instead: increment a dedicated `drop_count_below_base` diagnostic counter and forward the message. The downstream `store_buffer_write` will underflow the index and write to a wrapped or clamped address; the matching engine will observe an anomalous snapshot and can act on the toxicity signal. The host is expected to reconfigure `base_price` via `s_axilite` when the diagnostic counter spikes. Hard-dropping below-base messages is rejected — it removes the signal that `base_price` needs updating.

**Check 3 — Quantity floor (combinatorial):**
Extract the 32-bit quantity field. If `qty < min_qty` (configurable via `s_axilite`, default: 1, i.e. off by default), drop the message. One comparator.

The operational motivation is flow quality: the quantity floor is an informed/uninformed order discriminator. Informed traders — HFTs probing the book — post minimal size deliberately to minimise market exposure while still moving the best bid or ask. Uninformed and institutional flow arrives in size. A matching engine or spread-capture strategy wants to trade against uninformed flow; a passive limit order posted at a best price derived from genuine-size quotes captures the spread on every fill — a 100% win rate on executed passive orders. Admitting single-lot probing orders corrupts the best price signal, causing the matching engine to post at a price that will never be filled at size and eliminating the passive edge. The quantity floor filters out the probing layer before any book state is touched, preserving the integrity of the best price signal and with it the spread-capture guarantee. The ML batch job can calibrate `min_qty` per instrument per session from historical lot size distributions — liquid large-cap symbols may warrant `min_qty=100`, futures contracts may use their standard lot size.

All three checks execute in a single clock cycle (combinatorial after the 1-cycle LUTRAM read). `filter_event` is II=1 — it adds one cycle to the pipeline critical path.

**Drop counters:**
For each check, a saturating 32-bit counter increments on every dropped message. The counters are readable via `s_axilite` for diagnostics. They are not on the hot path (they are in the cold register file, updated only on drop).

**Output:**
If all checks pass, `filter_event` writes the payload to the `parse_add_event` input stream. If any check fails, nothing is written. The downstream stages block on their input stream until the next valid message arrives — this is correct behaviour in DATAFLOW (`ap_fifo` read blocks until data is available).

**Configuration registers (`s_axilite`):**
- `max_ticks_above_base` — price sanity upper bound (per instrument slot, default 2,000)
- `min_qty` — quantity floor (global, default 0 = disabled)
- `drop_count_membership` — read-only diagnostic counter (hard drop)
- `drop_count_price_high` — read-only diagnostic counter (hard drop, upper bound exceeded)
- `drop_count_below_base` — read-only diagnostic counter (soft signal, price below base_price — indicates stale base_price configuration)
- `drop_count_qty` — read-only diagnostic counter (hard drop)

## Consequences

**Pipeline latency:**
+1 cycle to end-to-end latency (the LUTRAM read for instrument membership). All other checks are combinatorial within the same cycle. At 250 MHz: +4 ns.

**Throughput:**
`filter_event` is II=1. It does not affect the II=1 target from ADR-013.

**Book correctness:**
Out-of-range price writes to BRAM (negative index, index beyond `BOOK_DEPTH`) are prevented. The `handle_event` / `store_buffer_write` stage can remove its own bounds check if `filter_event` is in place — one fewer comparator on the hot path.

**Drop counter diagnostics:**
Persistent counters allow the host to detect feed quality degradation without inspecting individual messages. A spike in `drop_count_price_high` during a session is only meaningful if `max_ticks_above_base` is correctly calibrated per instrument. On a volatile symbol with an under-calibrated threshold, the counter fires on legitimate orders — the spike indicates a configuration problem, not a data quality problem. The ML batch job is responsible for keeping the threshold above the instrument's expected intraday range; if the counter spikes despite a calibrated threshold, it indicates a genuine anomaly (fat-finger, data quality event). A spike in `drop_count_below_base` is an operational signal that `base_price` needs reconfiguration: the market has moved below the session-open base and the host should write a new `base_price` via `s_axilite`. All counters are saturating (not wrapping) to remain readable under sustained bad data.

**Dependency:**
Check 1 (instrument membership) reuses the ADR-014 LUTRAM routing table. `filter_event` depends on ADR-014's routing table being present. Checks 2 and 3 are independent and can be implemented before ADR-014 if needed.

## Alternatives Considered

**Inline checks in `parse_add_event`:**
Add guards inside `parse_add_event` that return early on invalid input. Does not require a new DATAFLOW stage. Problem: `parse_add_event` must still consume its input stream token regardless of outcome; it cannot conditionally suppress its output without a valid-flag side channel. The downstream `store_buffer_write` then needs to handle the valid-flag — complexity propagates through every stage. A dedicated filter stage that drops at the stream boundary is cleaner. Rejected.

**Filter on the host before writing to `ap_fifo`:**
The host process or DMA engine filters messages before writing to the kernel input FIFO. Zero FPGA resource cost. Problem: the host filter adds software latency before the FPGA hot path begins. Any message that passes the host filter but fails the FPGA filter is caught late. For a hardware pipeline where the value proposition is nanosecond-latency between feed and book, a host-side guard is architecturally inconsistent. Rejected.

**No filter (current state):**
Accept that anomalous messages produce anomalous book state; rely on the matching engine to sanity-check snapshot values before acting. Pushes validation responsibility downstream. An out-of-range BRAM write in the store buffer could silently corrupt a valid instrument's state. Beyond the data quality risk, the absence of a filter creates an adversarial exposure: a competing HFT firm can post a single-lot order at an extreme price, shift the computed mid-price, and cause the matching engine to post at an incorrect level — a standard quote manipulation technique. The quantity floor and price sanity checks directly close this vector. Rejected.
