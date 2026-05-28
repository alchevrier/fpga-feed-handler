# ADR-003 — DATAFLOW Pipeline Decomposition

## Status
Accepted

## Date
2026-05-28

## Context

A naive HLS implementation of the parse-to-book pipeline would be a single function reading an ITCH message, computing the new price level, and writing a snapshot. This works correctly but synthesises as a sequential state machine: the next message cannot begin parsing until the current message has completed its book write.

The throughput bottleneck in a sequential implementation is the longest stage. In this design, `handle_event` requires 4 cycles (2 BRAM read cycles + 1 compute + 1 BRAM write). A sequential implementation would have an initiation interval (II) of at least 4, meaning one message every 4 cycles minimum.

The target is **II=1 at the pipeline level** — one new message accepted per clock cycle — while keeping total latency at 6 cycles.

## Decision

The kernel is decomposed into three tasks connected by `hls::stream` channels, with `#pragma HLS DATAFLOW` applied at the top level:

```
┌─────────────────┐    hls::stream     ┌──────────────────┐    hls::stream     ┌───────────────────┐
│  parse_message  │ ──── MarketEvent ──▶│  handle_event    │ ──── LevelUpdate ──▶│ update_snapshot   │
│  II=1, 1 cycle  │                    │  II=1, 4 cycles  │                    │  II=1, 1 cycle    │
└─────────────────┘                    └──────────────────┘                    └───────────────────┘
```

**`parse_message`** (1 cycle, II=1):
- Reads raw bytes from the input `ap_fifo`
- Extracts `price`, `qty`, `side` fields from the ITCH Add Order message
- Writes a `MarketEvent` struct into the inter-task stream

**`handle_event`** (4 cycles, II=1):
- Reads current level from BRAM (2-cycle read latency)
- Computes updated quantity (1 cycle)
- Writes updated level back to BRAM (1 cycle)
- Writes a `LevelUpdate` into the snapshot stream

**`update_snapshot`** (1 cycle, II=1):
- Reads the `LevelUpdate` from the stream
- Updates the best bid/ask snapshot registers

**Design target: 6 cycles total latency** (1 + 4 + 1), pending HLS synthesis confirmation of II=1 on all three stages.

With `#pragma HLS DATAFLOW`, all three tasks execute concurrently on different messages. While `handle_event` processes message N, `parse_message` is already accepting message N+1. The pipeline achieves **II=1 at the top level** despite `handle_event` having a 4-cycle latency, because the 4-cycle stage is itself pipelined internally.

`hls::stream` channels enforce the producer-consumer ordering between tasks without shared mutable state. There is no lock. The FIFO depth absorbs the in-flight messages during pipeline fill.

## Alternatives Considered

**Single monolithic function**: correct but II=4 minimum, limited throughput. Rejected.

**Two tasks (parse + combined handle+snapshot)**: reduces inter-task channel count but couples BRAM access with register update in one stage, making the stage longer and harder to pipeline independently. Rejected.

## Consequences

- Each task must independently satisfy II=1 for the top-level II=1 guarantee to hold.
- `hls::stream` channels must be sized (FIFO depth) to prevent stalls. The DATAFLOW scheduler requires channels to be deep enough to buffer in-flight messages during pipeline fill (depth = pipeline latency of downstream stage).
- The decomposition makes each stage independently verifiable in simulation.
- Total pipeline latency is the sum of stage latencies: 1 + 4 + 1 = **6 cycles** (design target, pre-synthesis).

## Open Questions

**BRAM same-address hazard under pipeline load.**
With II=1, `handle_event` processes a new message every cycle. If two consecutive messages reference the same price level (e.g., two Add Orders at the same price), message N+1's BRAM read may issue before message N's BRAM write completes. Whether HLS inserts a read-after-write forwarding path automatically, stalls the pipeline, or produces incorrect results is not yet known. This is a primary question to answer from the HLS synthesis and schedule viewer report. If HLS cannot resolve it automatically, a forwarding register or a minimum II > 1 constraint on `handle_event` will be required.
