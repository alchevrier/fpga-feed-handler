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

ADR-012 introduces a register store buffer that absorbs `LevelUpdate` writes on the hot path, deferring BRAM writes to a cold path. This ADR documents the consequence: with BRAM removed from the hot path, II=1 becomes achievable across the full pipeline.

## Decision

Move `handle_event` BRAM writes to the cold path store buffer writer (ADR-012). The hot path `handle_event` equivalent becomes a single register write into the store buffer head — one cycle, pipelineable, no RAW hazard possible (the store buffer head is a ring; consecutive messages write to consecutive addresses by construction).

Revised hot path:

| Stage                  | Cycles | Pipelined | Notes                                                   |
|------------------------|--------|-----------|----------------------------------------------------------|
| `protocol_dispatch`    | 1      | II=1      | unchanged                                               |
| `arbitrate`            | 3      | II=1      | unchanged                                               |
| `filter_event`         | 1      | II=1      | unchanged                                               |
| `route_by_type`        | 1      | II=1      | unchanged                                               |
| `parse_add_event`      | 1–2    | II=1      | field extraction only (price, qty, side); DSP multiply  |
|                        |        |           | moves to cold path — idx not needed on the hot path     |
| `store_buffer_write`   | 1      | II=1      | register write, no BRAM, no RAW hazard                  |
| `update_snapshot`      | 1      | II=1      | unchanged                                               |

With all stages pipelined at II=1, the kernel II drops from 16 to 1. A new message can be admitted every clock cycle. Throughput: 250M messages/second at 250 MHz — matching the theoretical maximum of the `ap_fifo` interface.

End-to-end latency is not reduced by II improvement (latency is set by stage depths and DATAFLOW overlap), but throughput is no longer the bottleneck. For burst market data — multiple messages arriving in consecutive cycles — the improvement is the difference between processing them at 64 ns intervals vs 4 ns intervals.

**Expected latency post-refactor:**
With the DSP multiply and BRAM read-modify-write both removed from the hot path, the remaining stages contain no significant compute: field extraction (shifts and masks), register lookups, and register writes. The latency floor is no longer set by computation — it is set by the `ap_fifo` stream handshake: 1 cycle per stage. Each stage costs exactly 1 cycle to read from its input stream, with the single exception of `arbitrate` which retains its 3-cycle floor from the 64-bit sequence counter (compare latency=1 + add latency=1 + FIFO read=1). Theoretical hot-path latency: `(N_stages - 1) × 1 + 3` = ~9 cycles for the 7-stage pipeline. Synthesis is required to confirm — DATAFLOW scheduling and routing may add register stages — but the compute bottleneck is gone. The latency is now a structural property of the pipeline depth, not of any algorithm running inside it.

## Consequences

**Throughput:**
II=1 theoretical maximum. Burst market data processed without pipeline stall. This is the primary motivation.

**Latency:**
The refactor adds three new pipeline stages (`protocol_dispatch`, `filter_event`, `route_by_type`) and yet reduces end-to-end latency vs the current 13-cycle baseline. The reason: the DSP multiply (7 cycles) and BRAM read-modify-write (4 cycles) evicted from the hot path save 11 cycles; the three new 1-cycle stages cost 3. Net: −8 cycles. More features, lower latency — a direct consequence of moving compute off the hot path rather than optimising what is already there.

**BRAM depth accuracy:**
As documented in ADR-012, full depth in BRAM reflects market state as of the last flush. The matching engine consuming `BookSnapshot` registers is unaffected — it never reads BRAM directly.

**Verification:**
The existing CoSim testbench validates `BookSnapshot` correctness. It does not validate BRAM depth. A new testbench or testbench extension is required to validate cold-path BRAM contents after flush.

## Alternatives Considered

**BRAM with write forwarding on same-address RAW hazard:**
A forwarding register detects when two consecutive writes target the same BRAM address and bypasses the BRAM read for the second write. Achieves II=1 for the common case (different addresses). Adds a comparator and mux on the critical path; does not eliminate BRAM latency from the pipeline, only hides the RAW hazard. More complex than the store buffer approach with no latency advantage. Rejected.

**Increase BRAM port count (true dual-port):**
Use both ports of the RAM_2P for simultaneous read and write. Does not resolve the RAW hazard — the hazard is temporal (read before write completes), not a port contention issue. Rejected.

**Accept II=16:**
Current state. Adequate for single-message processing. Insufficient for burst processing — at 250M messages/second market data rates, bursts of 4–8 messages per microsecond are common at high-volume symbols. II=16 means the pipeline stalls for 64 ns per burst message. Rejected as the target architecture.
