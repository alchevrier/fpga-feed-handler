# ADR-008 — Scope: ADD Order Events Only

## Status
Accepted

## Date
2026-05-28

## Context

The NASDAQ ITCH 5.0 protocol defines six message types that modify the order book:

| Type | Message | Effect |
|---|---|---|
| `A` | Add Order (no MPID) | Add a new resting order at a price level |
| `F` | Add Order with MPID | Same as `A`, with market participant ID |
| `E` | Order Executed | Reduce qty at a level; remove if qty → 0 |
| `C` | Order Executed with Price | Execution at a non-displayed price |
| `X` | Order Cancel | Partial cancel — reduce qty at level |
| `D` | Order Delete | Full cancel — remove order entirely |

A complete ITCH order book requires handling all six types. The C++ reference implementation (`low-latency-feed-handler`) handles all of them.

However, the **architectural questions** this project exists to answer — deterministic BRAM addressing (ADR-005), DATAFLOW pipeline II=1 (ADR-003), clock-edge atomic snapshot consistency (ADR-006) — are fully answerable with Add Order alone. All message types exercise the same read-modify-write BRAM path. Adding `E`, `X`, `D` changes the arithmetic inside `handle_event` (subtract qty, conditionally zero the level) but does not change the pipeline structure, the addressing scheme, or the concurrency protocol.

The C++ reference implementation was also developed incrementally, beginning with Add Order only, then extending to cancel/modify/execute types after the core pipeline was established and benchmarked.

## Decision

The initial implementation supports **ITCH `A` (Add Order, no MPID) messages only**.

`F` (Add Order with MPID) is structurally identical to `A` with an additional 4-byte field and is a trivial extension once `A` is working.

`E`, `C`, `X`, `D` are deferred to a subsequent iteration after the synthesis report confirms the target II and latency.

## What "ADD Only" Means for the Design

- `parse_message` extracts: `price`, `qty`, `side` from the `A` message payload.
- `handle_event` performs: `new_qty = existing_qty + qty` (add, not subtract).
- `update_snapshot` refreshes the best bid/ask if the updated level improves the top of book.

The price level is never removed and qty never decreases in this scope. This means the snapshot best bid/ask can only move in one direction (tighter spread, more qty at best). This is a known simplification accepted for this phase.

## Consequences

- The testbench uses only `A` message bytes. Real ITCH captures contain all message types; the testbench must filter or use synthetic data.
- Cancel/execute/delete handling will require `handle_event` to branch on message type (qty increase vs. qty decrease vs. level removal). This is the primary extension point for the next iteration.
- The synthesis results (II, latency, resource utilisation) reported in this phase are valid for the Add-only pipeline. Extension to full message types may affect resource utilisation but is not expected to change II or total latency (same BRAM read-modify-write structure).
- The 6-cycle latency comparison against the C++ baseline (274 ns P99.9) is valid: the C++ baseline also measures Add Order parsing and insertion.
