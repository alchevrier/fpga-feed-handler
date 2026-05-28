# ADR-006 — Snapshot Registers: UltraFast Registers with Clock-Edge Atomicity

## Status
Accepted

## Date
2026-05-28

## Context

The order book pipeline writes updated price level data into BRAM (ADR-004). An external consumer — an order sender or risk monitor — needs to read the current best bid and best ask. This is the **snapshot**: a consistent view of the top of book at a point in time.

Two problems must be solved:

1. **Where to store the snapshot** (storage choice)
2. **How to provide a consistent read** to the external consumer without stalling the write pipeline (concurrency protocol)

### Problem 1 — Storage choice

The BRAM used by `handle_event` has limited ports: `ram_2p` provides one read port and one write port per clock cycle. The `handle_event` stage occupies both ports on every active cycle (read in cycles 1-2, write in cycle 4). A second reader on the same BRAM would require port arbitration, adding latency to the critical path or stalling the pipeline.

**UltraFast registers** (flip-flops in the FPGA fabric) have no port limitation: any number of readers can sample a register in the same clock cycle with no conflict. They have 1-cycle read latency (vs 2 cycles for BRAM). They are appropriate for small, frequently read state — exactly what a best-bid/best-ask snapshot is.

### Problem 2 — Consistent reads from an external consumer

The write pipeline (`update_snapshot`) updates the best bid and best ask as a multi-field record: `{bid_price, bid_qty, ask_price, ask_qty}`. On a CPU, an external reader could observe a torn write — new `bid_price` with old `bid_qty` — because load instructions execute sequentially over time and a writer can interleave between them.

Beyond structural consistency, there is a **compliance requirement**: the order sender must always act on the most current top of book. Sending an order priced against a stale best bid or ask is a best-execution violation. The read path must guarantee it never observes an in-progress or superseded snapshot — not as a latency goal, but as a hard correctness requirement.

On an FPGA this requirement is satisfied structurally. All flip-flop outputs are stable for the full clock period and are captured simultaneously on the rising edge. A reader sampling the snapshot registers in any clock cycle observes all four fields from the same clock edge — atomically by construction. No seqlock, no handshake, and no retry logic is required on the read path. The clock edge is the synchronisation primitive.

## Decision

The best bid/ask snapshot is stored in **UltraFast registers** (flip-flops in the FPGA fabric). The clock edge provides atomicity for reads — no seqlock, no handshake protocol, and no coordination overhead on either path.

### Write path (inside the pipeline — `update_snapshot` task)

```cpp
// Runs in update_snapshot, cycle 6 of the pipeline
snapshot.bid_price = new_bid_price;
snapshot.bid_qty   = new_bid_qty;
snapshot.ask_price = new_ask_price;
snapshot.ask_qty   = new_ask_qty;
```

### Read path (external consumer — order sender or risk monitor)

```cpp
// Direct register read — always coherent
BookSnapshot snap = snapshot;
```

### Why no seqlock is needed on the hardware read path

In the C++ reference implementation, the seqlock serves a specific purpose: with one writer thread and one reader thread, the reader must never observe an in-progress write — a snapshot where some fields have been updated and others have not. The odd sequence counter is a "write in progress" signal; the reader spins until the counter is even (write committed) before sampling. This guarantees the reader always sees the most up-to-date, fully committed top of book.

On an FPGA, clock-edge atomicity provides this guarantee structurally. A flip-flop's Q output only changes on the rising clock edge — there is no "between cycles" state. A write and a read cannot interleave within a cycle: either the read happens before the rising edge (reader sees previous committed snapshot) or after (reader sees new committed snapshot). There is no observable intermediate state, and no window in which the reader could catch a partial write. The reader always gets the most recently completed top of book with no spinning, no retry, and no coordination logic — the clock edge enforces what the seqlock was approximating in software.

**Ordering constraint:** a flip-flop's new Q output is only available from the cycle *after* the write commits. If the reader samples in the same cycle that `update_snapshot` writes, it observes the previous snapshot — not the new one. The freshness guarantee therefore requires the read to be scheduled at least one cycle after the write. In practice this is not a constraint: the external consumer (order sender or risk monitor) samples the snapshot periodically at a cadence far slower than the 4 ns clock period, and there is always at least one idle cycle between the write and the next sample.

**Note on HLS C simulation:** a `std::atomic` seqlock pattern may appear in the C simulation model to satisfy the HLS thread model, but it is a modelling artefact. The synthesised RTL does not require it — the clock edge provides the atomicity guarantee that software requires a seqlock to replicate.

## Why Not a Mutex or Handshake Signal

- **Mutex**: blocks the writer. The write pipeline must never stall on a reader's availability. Rejected.
- **Double-buffered snapshot with flip signal**: correct but requires two full copies of the snapshot register set plus a pointer switch. Adds resource cost and a one-cycle extra latency compared to a direct seqlock. Rejected.
- **AXI4-Lite read interface with handshake**: appropriate for a full SoC integration where the consumer is a processor. For this phase, a direct register read is simpler and sufficient. Deferred.

## Consequences

- The snapshot registers are declared outside the DATAFLOW region so HLS does not attempt to pipeline or partition them as BRAM.
- The external consumer reads directly — no retry logic, no spin loop, no coordination. Every read returns the latest completed snapshot.
- This design is **write-optimised and read-optimised**: neither path stalls or blocks the other at any point.
