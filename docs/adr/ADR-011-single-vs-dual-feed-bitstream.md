# ADR-011 — Single-Feed vs Dual-Feed Bitstream: Arbitration Cost Measured at +4 Cycles

## Status
Accepted

## Date
2026-06-01

## Context

The current kernel (`HEAD`) contains an `arbitrate` stage as the first task of the DATAFLOW pipeline. This stage maintains a MOLDUDP64 sequence counter and selects between two `ap_fifo` feeds (`feed_a`, `feed_b`), admitting exactly one message per cycle into the downstream pipeline. The rationale for this stage is documented in ADR-010.

Two synthesis + RTL co-simulation runs on the same device (`xa7a12t-cpg238-2I`, 4 ns clock) provide a direct measurement of the arbitration cost:

| Commit  | Design                        | Cycles | Latency | II | CoSim |
|---------|-------------------------------|--------|---------|----|-------|
| ef31052 | Single feed — no `arbitrate`  | 9      | 36 ns   | 12 | PASS  |
| HEAD    | Dual feed — `arbitrate` present | 13   | 52 ns   | 16 | PASS  |

**Arbitration cost: +4 cycles, +16 ns end-to-end.**

Both numbers are RTL co-simulation verified (Verilog/xsim). The 4-cycle overhead is not a pipeline stage count for `arbitrate` in isolation (which synthesises to 3 cycles) — it is the measured end-to-end delta after the HLS DATAFLOW scheduler has optimised stage overlap. The scheduler absorbs 3 cycles of arbitrate overlap against the downstream pipeline but cannot fully hide it; 4 cycles appear in the total latency.

The single-feed variant also has substantially lower resource utilisation:

| Metric      | Single feed (ef31052) | Dual feed (HEAD)  |
|-------------|----------------------|-------------------|
| FF          | 979 (6%)             | 3693 (23%)        |
| LUT         | 852 (10%)            | 1657 (20%)        |
| BRAM_18K    | 1 (2%)               | 1 (2%)            |
| DSP         | 2 (5%)               | 2 (5%)            |

The FF difference (−2714) reflects the 64-bit sequence counter, the seeded flag, the 64-bit comparator, and the 64-bit adder that `arbitrate` synthesises into. The LUT difference (−805) reflects the mux and control logic.

## Decision

Maintain two bitstreams compiled from the same source tree:

- **`kernel_single.bit`** — compiled with `DUAL_FEED=0` (or equivalent compile-time flag): `feed_b` port and `arbitrate` stage omitted. Single `ap_fifo` input (`feed_a`) wired directly to `parse_add_event`. **9 cycles, 36 ns.** Deployed when only a primary feed is connected — single-venue, single-line configurations.

- **`kernel_dual.bit`** — compiled with `DUAL_FEED=1` (default, current HEAD): `arbitrate` stage present, dual `ap_fifo` inputs. **13 cycles, 52 ns.** Deployed when primary and secondary redundant feeds are both wired up. Provides the compliance guarantee described in ADR-010.

The host selects the appropriate bitstream at deployment time based on physical wiring, not at runtime. There is no runtime switching between these two bitstreams — that is an operational configuration decision, not a hot-path event.

## Consequences

**Latency:**
Single-feed deployments gain 16 ns end-to-end versus the dual-feed kernel. At 250 MHz this is 4 full clock cycles — a meaningful improvement for latency-sensitive order generation. The dual-feed penalty is paid only when two feeds are physically present; deployments with a single line pay nothing.

**Resource utilisation:**
Single-feed kernel uses 6% FF and 10% LUT versus 23% FF and 20% LUT for dual-feed. The freed capacity is available for additional message types (X/D/U cancel/delete/replace) without approaching device limits on the xa7a12t.

**Correctness boundary:**
The single-feed kernel performs no sequence number validation — there is no `arbitrate` stage to reject gaps or duplicates. The host infrastructure (MOLDUDP64 gap detection, retransmission handling) bears full responsibility for feed integrity. This is the same responsibility model as the C++ reference implementation when running a single feed.

The dual-feed kernel retains the compliance guarantee: no message reaches `handle_event` until `arbitrate` has admitted it. See ADR-010 for the compliance argument.

**Build system:**
Both variants are built from the same `feed_handler.cpp` via a preprocessor constant controlling whether `arbitrate` and `feed_b` are compiled in. No logic duplication. A single `hls_config.cfg` per variant, differing only in the `-DDUAL_FEED` cflags.

## Production Context

In practice this decision is settled by jurisdiction and venue connectivity before latency enters the conversation:

- **Co-located at a US exchange (NYSE, NASDAQ, CBOE):** Colocation agreement provides guaranteed primary feed connectivity. MiFID II does not apply. Single-feed is the correct architecture — dual-feed adds 16 ns with no regulatory justification. `kernel_single.bit` is the production bitstream.

- **European venue (Euronext, XETRA, LSE) or any MiFID II-regulated workflow:** Best execution obligations effectively mandate dual-feed arbitration. The 16 ns cost is a regulatory floor, not an engineering choice. `kernel_dual.bit` is the only compliant option. ADR-010's compliance argument is the primary decision driver.

- **Multi-venue aggregation:** Arbitration is already happening at the venue-selection layer. Within-venue dual-feed overhead is secondary to the cross-venue latency problem.

The latency comparison in this ADR is therefore most useful as a **cost-of-compliance measurement** (16 ns is what MiFID II best execution costs at the feed layer on this device) and as a portfolio demonstration that per-stage pipeline costs are understood and measured. It is not a runtime operational decision in production.

## Alternatives Considered

**Single bitstream with runtime `feed_b` bypass:**
Wire `feed_b` to a dummy/always-empty FIFO when only one feed is present. The `arbitrate` stage remains in silicon unconditionally. Cost: 4 cycles and 2714 FF always paid, even on single-feed deployments. Rejected — the cost is fixed and the silicon footprint is wasted.

**Always compile dual-feed only:**
Accept 13 cycles as the baseline for all deployments. Valid if all target hardware has two feed connections. Rejected for this project — the measurement shows a real 16 ns cost and the single-feed case is a legitimate deployment target.
