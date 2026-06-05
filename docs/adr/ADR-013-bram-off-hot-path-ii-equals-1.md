# ADR-013 — BRAM Off Hot Path: Achieving II=1 on the Full Pipeline

## Status
Proposed — Deferred (not yet implemented)

## Date
2026-06-04

## Context

The current DATAFLOW pipeline achieves II=16 with the following per-stage costs:

| Stage                | Cycles | Pipelined | Notes                              |
|----------------------|--------|-----------|------------------------------------|
| `arbitrate`          | 3      | II=1      | 64-bit seq counter floor           |
| `parse_add_event`    | 7      | II=1      | DSP multiply latency               |
| `handle_event`       | 4      | no        | BRAM read-modify-write, RAW hazard |
| `update_snapshot`    | 1      | II=1      | register compare/write             |

The II=16 bottleneck is `handle_event`. As a non-pipelined stage with a 4-cycle latency, it sets the kernel interval — a new message cannot be admitted until the BRAM write completes. At 250 MHz, II=16 = 64 ns minimum throughput, regardless of the 13-cycle latency.

The RAW hazard is structural: two consecutive messages that update the same BRAM address cannot be pipelined because the second read would observe stale data from before the first write completes. This hazard cannot be resolved by pragma or scheduling — it is a property of the BRAM port model.

ADR-012 introduces a 16-register eviction-based book (8 bids, 8 asks) that replaces the BRAM read-modify-write on the hot path. The register file holds the top-8 levels per side; BRAM receives write-only eviction writes when a level is displaced from the register file. This ADR documents the consequence: with BRAM reads removed from the hot path and the only remaining BRAM access being write-only, the RAW hazard is eliminated and II=1 becomes achievable across the full pipeline.

## Decision

Replace `handle_event` with a `register_book_update` stage (ADR-012). The hot path becomes: scan the 8-slot register file for a price match (8-wide comparator tree, combinatorial), update qty in-place on hit, or on miss find the worst slot (7-comparator tree, combinatorial), evict it to BRAM (write-only), and insert the new level. No BRAM read occurs on the hot path. Write-only BRAM access has no RAW hazard — consecutive evictions at different addresses are independent.

Revised hot path:

| Stage                  | Cycles | Pipelined | Notes                                                   |
|------------------------|--------|-----------|----------------------------------------------------------|
| `protocol_dispatch`    | 1      | II=1      | unchanged                                               |
| `arbitrate`            | 3      | II=1      | unchanged                                               |
| `filter_event`         | 1      | II=1      | unchanged                                               |
| `route_by_type`        | 1      | II=1      | unchanged                                               |
| `parse_add_event`      | 1–2    | II=1      | field extraction only (price, qty, side); DSP multiply  |
|                        |        |           | moves to cold path — idx not needed on the hot path     |
| `register_book_update` | 7–8    | II=1      | comparator trees (combinatorial) + DSP for eviction BRAM addr; write-only BRAM, no RAW hazard |
| `update_snapshot`      | 1      | II=1      | unchanged                                               |

With all stages pipelined at II=1, the kernel II drops from 16 to 1. A new message can be admitted every clock cycle. Throughput: 250M messages/second at 250 MHz — matching the theoretical maximum of the `ap_fifo` interface.

End-to-end latency is not reduced by II improvement (latency is set by stage depths and DATAFLOW overlap), but throughput is no longer the bottleneck. For burst market data — multiple messages arriving in consecutive cycles — the improvement is the difference between processing them at 64 ns intervals vs 4 ns intervals.

**Expected latency post-refactor:**
The BRAM read-modify-write (4 cycles) is gone. `register_book_update` introduces comparator trees (combinatorial) and a DSP multiply for the eviction BRAM address (7 cycles); this DSP latency is the new stage depth, but it is fully hidden by II=1 — DATAFLOW overlaps its execution with the next stage. The `parse_add_event` hot path remains field-extract only (1–2 cycles); the DSP fires only on eviction within `register_book_update`, not on every message. Theoretical hot-path latency for the 7-stage pipeline: ~9–10 cycles at 250 MHz ≈ 36–40 ns. Synthesis is required to confirm — the register file dependency (consecutive messages reading and writing `bids[8]`/`asks[8]`) requires complete array partitioning and HLS forwarding resolution to achieve II=1. The compute bottleneck is eliminated; the remaining latency is a structural property of pipeline depth.

## Consequences

**Throughput:**
II=1 theoretical maximum. Burst market data processed without pipeline stall. This is the primary motivation.

**Latency:**
The refactor adds three new pipeline stages (`protocol_dispatch`, `filter_event`, `route_by_type`) and yet reduces end-to-end latency vs the current 13-cycle baseline. The reason: the DSP multiply (7 cycles) and BRAM read-modify-write (4 cycles) evicted from the hot path save 11 cycles; the three new 1-cycle stages cost 3. Net: −8 cycles. More features, lower latency — a direct consequence of moving compute off the hot path rather than optimising what is already there.

**BRAM depth accuracy:**
With the eviction model (ADR-012), BRAM is written immediately on each eviction — there is no flush interval, no staleness window. BRAM always reflects the state of every price level that has ever left the top-8 register file. The matching engine consuming `BookSnapshot` registers is unaffected — it reads only from the register file, never from BRAM.

**Verification:**
The existing CoSim testbench validates `BookSnapshot` correctness. It does not validate BRAM depth. A new testbench or testbench extension is required to validate BRAM contents after eviction — specifically, that evicted levels are written to the correct BRAM address and that the register file correctly tracks the top-8 levels after each eviction.

## Alternatives Considered

**BRAM with write forwarding on same-address RAW hazard:**
A forwarding register detects when two consecutive writes target the same BRAM address and bypasses the BRAM read for the second write. Achieves II=1 for the common case (different addresses). Adds a comparator and mux on the critical path; does not eliminate BRAM latency from the pipeline, only hides the RAW hazard. More complex than the store buffer approach with no latency advantage. Rejected.

**Increase BRAM port count (true dual-port):**
Use both ports of the RAM_2P for simultaneous read and write. Does not resolve the RAW hazard — the hazard is temporal (read before write completes), not a port contention issue. Rejected.

**Accept II=16:**
Current state. Adequate for single-message processing. Insufficient for burst processing — at 250M messages/second market data rates, bursts of 4–8 messages per microsecond are common at high-volume symbols. II=16 means the pipeline stalls for 64 ns per burst message. Rejected as the target architecture.
