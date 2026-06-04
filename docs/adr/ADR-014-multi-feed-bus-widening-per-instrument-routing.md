# ADR-014 — Multi-Feed Bus Widening and Per-Instrument Pipeline Routing

## Status
Proposed — Deferred (not yet implemented)

## Date
2026-06-04

## Context

The current kernel accepts two `ap_fifo` feeds (`feed_a`, `feed_b`) carrying ITCH Add Order messages for a single instrument (ASML). The `arbitrate` stage selects the valid message by MOLDUDP64 sequence number and emits it to a single downstream pipeline.

Production market data infrastructure handles multiple instruments simultaneously on the same physical feed. A single NASDAQ direct feed carries messages interleaved across all 8,669+ symbols. The current design processes only pre-filtered single-instrument data — the upstream filtering is assumed to happen before the kernel boundary.

To move toward a more realistic architecture, the kernel needs to:

1. Accept N feeds (N = number of physical connections, e.g. 4: two venues × primary/secondary)
2. Dispatch each feed slot to the correct protocol pipeline (ITCH, FIX, OUCH) before any protocol-specific logic runs
3. Route messages within the ITCH pipeline to the correct per-instrument pipeline based on the `stock_locate` field
4. Maintain separate order book state per instrument without cross-instrument interference

Both the protocol assignment and the instrument routing table must be configurable at startup without bitstream recompilation, as feed slot assignments and tracked instrument sets change between trading sessions.

## Decision

**Bus widening:**
Widen the input bus from 2 feeds to N feeds by extending the `ap_fifo` array. N is a compile-time constant set at bitstream build time (e.g. N=4 for two venues × primary/secondary). Each feed slot carries an opaque fixed-width byte payload — the bus width is chosen to accommodate the widest expected message across all protocols (e.g. `ap_uint<352>`). Feed slots do **not** share a common framing format: NASDAQ ITCH equity feeds use MOLDUDP64 framing, NASDAQ options feeds use a different framing, FIX/OUCH have their own wire formats. The `protocol_dispatch` stage knows the framing for each slot from the `feed_protocol[N]` register written via `s_axilite` at startup; it passes the raw payload to the pipeline that understands that slot's format. No stage upstream of `protocol_dispatch` inspects or assumes the payload structure.

**Protocol dispatch via `s_axilite` feed protocol register:**
The host writes a per-slot protocol register at startup via `s_axilite`:
```
feed_protocol[N]  // e.g. PROTOCOL_ITCH=0, PROTOCOL_FIX=1, PROTOCOL_OUCH=2
```
`protocol_dispatch` reads `feed_protocol[slot_index]` — a single register read, no payload inspection — and routes the message to the corresponding protocol pipeline. This is the first stage in the pipeline: routing happens before any protocol-specific parsing, so each protocol pipeline is completely independent.

The register is written once before the first message and does not change during the trading session. Example configuration: slots 0–1 = `PROTOCOL_ITCH` (NASDAQ primary/secondary), slots 2–3 = `PROTOCOL_FIX` (broker OMS).

**`arbitrate` — per-protocol, not global:**
Each protocol pipeline owns its own `arbitrate` stage with the mechanism appropriate to that protocol. For ITCH: MOLDUDP64 sequence number selection (current implementation). For FIX/OUCH: protocol-specific sequencing. `arbitrate` is extended from 2-feed to N-feed within the ITCH pipeline by receiving only the slots assigned `PROTOCOL_ITCH` by `protocol_dispatch`.

Per-slot expected sequence number initialisation is also written via `s_axilite` (replaces the current single `init_seq` parameter).

**Per-instrument pipeline routing:**
After arbitration, the `stock_locate` field is used to index a LUTRAM routing table (stock_locate → pipeline slot index) written at startup via `s_axilite`. Each instrument occupies one pipeline slot; the routing table maps the 16-bit stock_locate to a compact slot index. Instrument state (snapshot registers, store buffer) is indexed by slot, not by stock_locate directly, to keep the hot-path index width small.

Each pipeline slot is an independent instance of `filter_event → route_by_type → parse_* → store_buffer_write → update_snapshot`.

```
feed_0 ─┐
feed_1 ─┤
feed_2 ─┤──▶ protocol_dispatch ──▶ arbitrate (ITCH slots only)
feed_3 ─┘         │                      │
                   │               route_by_instrument
              FIX/OUCH/...         ──▶ pipeline_slot[0] ──▶ snapshot[0]
              (other repos)        ──▶ pipeline_slot[1] ──▶ snapshot[1]
                                   ──▶ pipeline_slot[k] ──▶ snapshot[k]
```

**Instrument count:**
The number of simultaneously tracked instruments (K) is a compile-time constant. K=8 is a reasonable starting point for the xa7a12t given resource constraints. Each additional instrument adds one pipeline slot instance and one set of snapshot registers.

## Consequences

**Routing table latency:**
The stock_locate → slot lookup is a LUTRAM read (1 cycle, single-port, no BRAM required for small K). It adds one cycle to the hot path between `arbitrate` output and `parse_add_event`. This is a pipeline register, not a stall.

**Resource scaling:**
Resources scale linearly with K (pipeline instances) and logarithmically with N (arbitration mux width). The xa7a12t at current utilisation (23% FF, 20% LUT after ADR-012/013 refactor expected to reduce significantly) has headroom for K=4–8 instrument slots.

**`s_axilite` interface growth:**
The AXI-Lite control map grows with N (per-feed init_seq) and K (routing table entries). This does not affect hot-path timing but increases host configuration complexity. A host-side configuration library is required.

**Sequence counter per feed:**
The single `expected_seq` static counter in the current `arbitrate` becomes N independent counters, one per feed slot. Each feed's sequence is tracked independently — a gap on the secondary feed does not affect the primary feed's counter.

**Single-instrument compatibility:**
Setting N=2, K=1 and the routing table to a single entry reproduces the current design exactly. The multi-instrument architecture is a strict superset.

## Alternatives Considered

**Software pre-filtering (current approach):**
The host or a dedicated pre-filter process filters the raw feed to single-instrument before writing to the FPGA `ap_fifo`. Simple, no routing logic in hardware. Limitation: the filter runs on CPU, adding software latency before the FPGA hot path begins. Acceptable for the current single-instrument prototype; not scalable to multi-instrument.

**Hash-based routing (stock_locate hash → slot):**
Replace the LUTRAM lookup with a hash function on stock_locate. Eliminates the lookup table but requires collision resolution logic and cannot guarantee 1-cycle routing for all inputs. Rejected — LUTRAM lookup is simpler, deterministic, and sufficient for small K.

**Single pipeline, time-multiplexed across instruments:**
One pipeline instance, instrument state stored in BRAM indexed by stock_locate. Eliminates per-instrument pipeline replication. Returns BRAM to the hot path (ADR-013 motivation). Rejected.
