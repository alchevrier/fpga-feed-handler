# ADR-010 — Multi-Feed Arbitration: Hardware Signal Mux vs MPSC Post-Hoc Validation

## Status
Accepted

## Date
2026-06-01

## Context

Production exchange connectivity commonly requires consuming two independent feeds of the same market data — either two redundant lines from the same exchange (primary/backup), or feeds from two separate venues that must be merged into a single order book. The critical design question is: **at what point in the pipeline is the decision made about which feed's message is valid and admitted to the book?**

This question has two dimensions. The first is performance: how many nanoseconds does the arbitration decision cost? The second — and the primary motivation for this design — is **compliance**: MiFID II best execution obligations require that any order generated from the book is based on correct market data at the moment of generation. A CPU MPSC architecture cannot satisfy this requirement by design, because the window during which the book holds an incorrect state is bounded below by the starvation guard expiry of the correcting feed — an indeterminate interval that is not configurable away. The FPGA implementation eliminates this window at the architectural level, before any software policy or operational control is considered.

The current kernel handles a single `ap_fifo` feed. Adding a second feed (`feed_b`) exposes a fundamental architectural difference between FPGA and CPU pipelines.

**CPU MPSC approach (N-SPSC → MPSC):**

In the C++ reference implementation, each feed is parsed by a dedicated producer thread (N-SPSC into a shared MPSC queue). The book writer thread is the single consumer. The MPSC queue model means:

1. Feed A's parser thread writes a parsed message into the queue (atomic release store).
2. Feed B's parser thread does the same concurrently.
3. The book writer dequeues the next available message (atomic acquire load, coherency round-trip).
4. **Only now** — inside the consumer, after the message has already traversed the queue — does the consumer check whether the message is valid: correct sequence number, not a duplicate of a message already applied from the other feed, not a retransmission artefact.

If the message fails validation, the book writer discards it. But the coherency cost, the queue slot, and the consumer's spin cycle have already been paid. The correctness check is **post-hoc** — deferred to the end of the pipeline, after resources have been consumed.

**FPGA signal arbitration:**

With two `ap_fifo` ports (`feed_a`, `feed_b`), a dedicated `arbitrate` task can inspect both feed inputs and select the winning message **before any downstream stage is invoked**. The selection is a combinatorial mux on wire-level signals: sequence number comparison, feed priority, or simple first-valid wins. No book state is touched until arbitration has already committed to a single message.

```
feed_a ──┐
          ├──▶ arbitrate ──hls::stream<ap_uint<288>>──▶ parse_add_event ──▶ handle_event ──▶ update_snapshot
feed_b ──┘
         (signal mux, 1 cycle)
```

The arbitration decision is temporally coincident with message arrival. The downstream pipeline — parse, book insert, snapshot — is invoked exactly once per valid admitted message. A rejected message from either feed never reaches `handle_event`; the book cannot be in a state where it applied a message that was later found invalid.

## Decision

Extend the kernel to accept two `ap_fifo` input ports and add an `arbitrate` task as the first stage of the DATAFLOW pipeline. The arbitrate task implements feed selection logic (sequence number comparison or priority) and emits exactly one message per decision cycle into the existing parse → handle → snapshot pipeline.

The `ap_fifo` stream is widened to `ap_uint<352>` (288-byte ITCH payload + 64-bit MOLDUDP64 sequence number prepended by the host). `arbitrate` maintains a `static` expected sequence counter — the only state that must survive across kernel invocations. The host writes the initial sequence number once at startup via `s_axilite`; thereafter `arbitrate` owns the counter and the host never touches it again.

```cpp
void arbitrate(
    hls::stream<ap_uint<352>>& feed_a,   // bits [351:288] = MOLDUDP64 seq,
    hls::stream<ap_uint<352>>& feed_b,   // bits [287:0]   = ITCH payload
    hls::stream<ap_uint<288>>& out,
    const ap_uint<64>          init_seq  // s_axilite, written once at startup
) {
#pragma HLS PIPELINE II=1

    static ap_uint<64> expected_seq = 0;
    static bool        seeded       = false;

    if (!seeded) {
        expected_seq = init_seq;
        seeded = true;
    }

    // Combinatorial circuit — truth table:
    //   feed.empty() | seq == expected_seq | output
    //       1        |          X          |  no write (feed idle)
    //       0        |          0          |  no write (gap/duplicate discarded)
    //       0        |          1          |  write to out (message admitted)
    // Inputs: FIFO-empty flag, seq comparator. Output: downstream FIFO write enable.
    auto admit = [&](hls::stream<ap_uint<352>>& feed) -> bool {
        if (feed.empty()) return false;
        ap_uint<352> raw = feed.read();
        ap_uint<64>  seq = raw.range(351, 288);
        ap_uint<288> msg = raw.range(287,   0);
        if (seq == expected_seq) {
            out.write(msg);           // admitted — downstream pipeline invoked
            ++expected_seq;
            return true;
        }
        return false;                 // gap or duplicate — discarded, book untouched
    };

    if (!admit(feed_a))
        admit(feed_b);
}
```

The sequence number is sourced from the MOLDUDP64 transport header, which wraps each ITCH datagram. It is stripped from the payload before entering the kernel and prepended to the `ap_uint<352>` word by the host-side DMA logic, keeping the ITCH payload boundary clean inside `parse_add_event`. The `static` counter advances only on clean admits — a gap increments nothing, so feed B's corrective message (carrying the expected sequence number) is admitted on arrival without any additional signalling.

## Consequences

**Correctness point moves upstream:**
On CPU, the validity check runs inside the book writer thread, after queue traversal. On FPGA, the validity check runs in `arbitrate`, before `parse_add_event`. The book writer (`handle_event`) receives only pre-validated messages. The invariant is enforced by the pipeline topology, not by a runtime conditional inside the consumer.

**No wasted pipeline resources on rejected messages:**
A discarded message from either feed costs 1 cycle in `arbitrate` and touches no BRAM, no snapshot registers, and no downstream logic. On CPU, a discarded message has already consumed: producer atomic store, coherency round-trip, consumer dequeue, and the validation branch — all before the discard decision.

**Deterministic book state:**
Because arbitration precedes parsing, `handle_event` can assume every incoming `MarketEvent` is already arbitrated and sequence-validated. No conditional rollback or compensating write is needed in the book writer.

**Compliance window:**
On CPU, if a corrupted or out-of-sequence message from feed A is applied to the book before feed B's corrective message arrives, the book enters an incorrect state. It remains incorrect for the duration of the starvation guard expiry on feed B's consumer — an indeterminate interval during which any order generated from the book's snapshot is based on incorrect prices or quantities. Under MiFID II best execution obligations (and equivalent frameworks), an order sent during this window may constitute a compliance breach: the firm cannot demonstrate that execution was on terms most favourable to the client because the reference data was provably wrong at the time of order generation. On FPGA, the book is never written before arbitration completes. The incorrect state window has zero duration by construction — there is no interval during which the book holds a value that feed B would have overridden.

**Latency overhead:**
`arbitrate` is `#pragma HLS PIPELINE II=1` — it adds 3 clock cycles (12 ns at 250 MHz) to the pipeline depth. The extra depth beyond 1 cycle comes from the `static seeded` flag: HLS must sequence the init check before the admit logic, adding pipeline stages. Total kernel latency is 13 cycles (52 ns), verified by RTL co-simulation.

## Comparison

| Property | CPU N-SPSC → MPSC | FPGA signal mux + DATAFLOW |
|---|---|---|
| Arbitration point | Consumer thread, after queue traversal | `arbitrate` task, before parse |
| Book touched on invalid message | Yes (discard happens post-dequeue) | No (arbitrate discards before parse) |
| Coherency cost on rejected message | Full round-trip paid | Zero — wire mux, no queue |
| Latency of arbitration decision | Coherency bound (40–300 ns depending on NUMA) | 3 clock cycles (12 ns) |
| Book state invariant | Enforced by runtime check in consumer | Enforced by pipeline topology |
| **Compliance exposure window** | **Indeterminate — starvation guard on feed B sets the floor** | **Zero — book never written before arbitration** |
| **Cost of arbitration** | **Indeterminate — coherency + starvation guard, no upper bound** | **Exactly 1 clock cycle — no upper bound exists** |

## Alternatives Considered

**Single feed with software deduplication:**
Keep one `ap_fifo`, merge feeds externally before the kernel. Pushes the arbitration problem upstream out of the FPGA; does not solve the latency or correctness ordering problem. Rejected — arbitration latency remains in the CPU path.

**Two parallel kernel instances:**
Run one kernel per feed, let both write to the book. Requires inter-kernel coordination to prevent duplicate book updates — the exact problem being avoided. Rejected.

**Arbitrate inside `parse_add_event`:**
Merge arbitration into the parse stage to save a pipeline stage. Rejected — conflating feed selection with message parsing reduces clarity and makes it harder to swap arbitration policy without modifying the parse stage.

**CPU mitigation: alternate starvation guard per message:**
On CPU, the compliance window could theoretically be reduced by checking both feeds on every consumer iteration rather than falling back to feed B only after feed A's slot is empty. This eliminates the case where feed B's corrective message waits behind feed A's starvation guard. However, it requires the consumer to unconditionally attempt a dequeue from both feeds on every message — paying the starvation guard cost twice per iteration regardless of whether feed B has data. At high message rates this collapses P50 throughput. The tradeoff is binary on CPU: either accept the compliance window, or accept the performance penalty. There is no configuration that eliminates both. Rejected as a viable production design; noted here to close the objection.

## Summary

The fundamental asymmetry is: **FPGA arbitration costs exactly 1 clock cycle regardless of load, feed count, or system state. CPU MPSC arbitration has no guaranteed upper bound** — it is bounded below by the coherency protocol (40–300 ns depending on NUMA distance) and has no ceiling once starvation guard backoff activates. In a two-feed scenario this is not a constant-factor difference — it is the difference between a deterministic cost and an unbounded one.

A second asymmetry compounds this over time: **feature cost scales differently on each architecture.** On CPU, every new feature (additional feed, new validation rule, new message type) adds sequential work to the consumer's hot path — the P50 grows with each addition. On FPGA, each new feature is an additional pipeline stage that executes in parallel with adjacent stages; the incremental cost is exactly 1 clock cycle per stage, and that cycle is already occupied by the previous message. The FPGA P50 stays flat as feature count grows. The CPU P50 does not. At some feature count — which this project will cross — the CPU P50 exceeds the FPGA P50 entirely, not just at the tail.

The third and primary asymmetry is compliance: **a CPU MPSC implementation cannot guarantee zero-duration incorrect book state by design.** The FPGA implementation can and does — not as a consequence of careful software engineering, but as a consequence of the pipeline topology. Implementing the order book on FPGA is therefore a compliance decision first, and a performance decision second.
