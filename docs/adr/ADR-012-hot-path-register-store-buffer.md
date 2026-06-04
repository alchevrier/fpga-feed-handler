# ADR-012 — Hot-Path Write Coalescing: Register Store Buffer

## Status
Proposed — Deferred (not yet implemented)

## Date
2026-06-04

## Context

The current `handle_event` stage maintains full order book depth in a static BRAM array. Every admitted message triggers a BRAM read-modify-write: read the existing level, add the new quantity, write back. This is 4 cycles, non-pipelined, and sits on the hot path between `arbitrate` and `update_snapshot`.

The matching engine consuming the output of this kernel does not need full order book depth in real time. It needs the best bid and best ask — the `BookSnapshot` registers that `update_snapshot` maintains. Full depth (all price levels and quantities) is useful for toxicity analysis and risk monitoring, but on a timescale of hundreds of thousands of cycles, not per-message.

More fundamentally, a matching engine is delta-driven: it reacts to each `LevelUpdate` event as it arrives — price, quantity, side, and action — and maintains its own internal state from the stream of deltas. It does not poll a depth snapshot to decide whether to generate an order; it acts on the incremental change. This means the hot path obligation is to deliver the delta with minimum latency, not to guarantee that a separately maintained full-depth BRAM is current at the moment the matching engine reads it.

Cold-path consumers (toxicity monitor, risk system) can similarly read directly from the register store buffer — the most recent N entries are always in registers, accessible without BRAM arbitration. This makes the store buffer the live data source for all consumers, hot and cold alike. BRAM is then reduced to a pure persistence artifact: end-of-day book state reconstruction and crash recovery. It is written once per flush interval not because any running process needs it, but so the state is not lost if the kernel is reset.

This creates an asymmetry: the hot path pays a 4-cycle BRAM penalty every message to maintain data that the downstream consumer reads on a cold timescale.

**Market self-invalidation property:**
Market data naturally chases stale quotes. A price level that was quoted 250,000 cycles ago will either have been traded through, cancelled, or revised by new messages before any risk system needs to read it. This means the store buffer does not require LRU eviction logic — incoming messages overwrite stale entries by natural market activity. The buffer does not need to decide what to evict; the market decides for it.

## Decision

Replace the single-stage BRAM read-modify-write with a two-path architecture:

**Hot path — register store buffer:**
A small SPSC ring of registers (4–8 entries, sized to absorb burst arrivals) sits between `parse_add_event` and `update_snapshot`. Each admitted message writes its `LevelUpdate` (price, qty, side) into the head register. No BRAM access on the hot path. The snapshot update logic reads directly from the head register — one register read, one compare, one conditional write. All operations are single-cycle, pipelineable to II=1.

**Cold path — BRAM writer:**
A separate process drains the store buffer tail to BRAM on either of two conditions, whichever fires first: M admitted messages (default M=1024, configurable via `s_axilite`) or T cycles elapsed since the last flush (default T=250,000,000 cycles = 1 s at 250 MHz, configurable via `s_axilite`). The dual trigger is necessary: the message-count trigger ensures each flush during active trading reflects a meaningful quantum of market activity rather than firing mid-burst; the cycle watchdog ensures BRAM is flushed during quiet periods where message rates may be near zero and the count threshold would never be reached — a mid-day trading halt, a news-driven circuit breaker, or a liquidity gap can silence a symbol for minutes or longer, and a kernel reset during that window would lose all order book state accumulated since the last message-count flush, with no recovery path. If the same price level appears multiple times in the buffer before the flush, the latest entry overwrites earlier ones — no comparison logic, just positional overwrite. The cold path runs asynchronously and never blocks the hot path.

```
Hot path:
  arbitrate → parse_add_event → [store buffer head] → update_snapshot
                                        ↓ (async, every N cycles)
Cold path:
  [store buffer tail] → BRAM writer → depth BRAM
```

The store buffer is implemented as a small ring of flip-flop registers (not BRAM, not LUTRAM) to guarantee single-cycle read/write access with no address computation on the hot path.

## Consequences

**Hot path latency:**
BRAM access and address computation are removed from the hot path. See ADR-013 for the measured II impact.

**Depth accuracy and consumer model:**
The store buffer is the live data source for all consumers. The matching engine reacts to the delta stream; cold-path consumers (toxicity monitor, risk system) read the most recent register entries directly — no BRAM arbitration, no staleness concern within the buffer window. A risk engine in particular cannot tolerate a BRAM read round-trip before firing a cancel: the cancellation decision must be taken on the delta that triggered the breach, not after polling a stale depth view. BRAM is a persistence layer only: end-of-day book state reconstruction and crash recovery. Any downstream system that needs full historical depth reads BRAM post-session, not during the trading day.

**Burst absorption:**
The store buffer absorbs message bursts without blocking `parse_add_event`. If the buffer fills (burst exceeds buffer depth before the cold path drains it), the oldest entry is overwritten — consistent with the market self-invalidation property. Overflow is not an error; it is expected market behaviour.

**Flush trigger:**
Hybrid: message-count primary (M=1024, configurable), cycle watchdog fallback (T=250M cycles = 1 s at 250 MHz, configurable). During active trading the message-count trigger fires first; during quiet periods (pre-market, post-close) the watchdog ensures BRAM is flushed at least once per second regardless of message rate. Both thresholds are written via `s_axilite` at startup. The cold path is not on the hot path critical section and its latency is irrelevant to the matching engine.

**ML-driven configuration lifecycle:**
The relevance of BRAM depth data is not fixed — a symbol that was informative on Monday may be irrelevant noise on Tuesday. A post-session ML batch job can analyse the captured BRAM data overnight (which price levels were actually traded through, which deltas preceded fills, which depth snapshots correlated with toxicity signals) and produce updated flush thresholds per instrument for the next session: higher M for liquid symbols that self-invalidate quickly, lower M or shorter T for illiquid symbols where each message is more persistent. These thresholds are written to `s_axilite` registers at session open — same bitstream, learned configuration. The FPGA hardware is invariant; only the operational parameters adapt.

**Resource impact:**
8-entry register buffer at `LevelUpdate` width (72 bits: 32 price + 32 qty + 8 side) = 576 flip-flops. Negligible on xa7a12t.

## Alternatives Considered

**LRU eviction from store buffer:**
Requires N-wide timestamp comparators to find the least recently used entry. Expensive in LUTs, on the critical path, and unnecessary given the market self-invalidation property. Rejected.

**Flush on pressure (when buffer full) instead of fixed message count:**
More principled — the cold path drains exactly when needed. Adds a full/empty comparison on the hot path (one extra cycle of combinatorial logic). Viable alternative; message-count triggering chosen for simplicity of the first implementation. Can be revisited.

**Keep BRAM on hot path, reduce latency by pipelining the read-modify-write:**
BRAM read latency is fixed at 2 cycles on 7-series. Pipelining does not remove the latency, it hides it — but only if II=1 is achievable with back-to-back messages updating different addresses. Same-address updates create a RAW hazard that the scheduler cannot resolve without forwarding logic. Rejected — store buffer is cleaner.
