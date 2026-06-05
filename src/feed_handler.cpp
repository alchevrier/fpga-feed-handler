#include "feed_handler.hpp"

// arbitrate: conditionally writes ap_uint<288> (ITCH payload) on seq match.
// HLS C-sim may warn about read() on empty stream — this is conservative static
// analysis. In CoSim and hardware, parse_add_event blocks on the FIFO until
// arbitrate admits a message; that is normal back-pressure, not a deadlock.
void arbitrate(
    hls::stream<ap_uint<352>>& feed_a,
    hls::stream<ap_uint<352>>& feed_b,
    hls::stream<ap_uint<288>>& out
) {
#pragma HLS PIPELINE II=1

    // 32-bit seq: 64-bit compare + add cost 2 cycles on Artix-7 LUT chain.
    // 32-bit closes in 1 cycle. Max NASDAQ session: 163M msgs << 2^32 = no wrap.
    static ap_uint<32> expected_seq = 1;

    if (!feed_a.empty()) {
        ap_uint<352> raw = feed_a.read();
        if (raw.range(319, 288) == expected_seq) {
            out.write(raw.range(287, 0));
            ++expected_seq;
            return;
        }
    }
    if (!feed_b.empty()) {
        ap_uint<352> raw = feed_b.read();
        if (raw.range(319, 288) == expected_seq) {
            out.write(raw.range(287, 0));
            ++expected_seq;
        }
    }
}

// filter_event: price sanity + quantity floor. II=1, always reads.
// Drops messages that would corrupt book state or degrade signal quality.
// Conditionally writes to out — downstream parse_add_event blocks on ap_fifo
// until a valid message arrives (correct DATAFLOW back-pressure behaviour).
//
// Check 1 — Price upper bound (hard drop):
//   price > base_price + max_ticks * tick_size → BRAM index overflow
// Check 2 — Price lower bound (diagnostic drop):
//   price < base_price → index underflow; drop_count_below_base signals stale config
// Check 3 — Quantity floor (hard drop):
//   qty < min_qty → sub-minimum noise order; corrupts best-price signal
//
// Instrument membership (ADR-014 LUTRAM) deferred — added with routing table.
static void filter_event(
    hls::stream<ap_uint<288>>& in,
    hls::stream<ap_uint<288>>& out,
    const ap_uint<32>          filt_base,
    const ap_uint<32>          filt_max,
    const ap_uint<32>          filt_minqty,
    ap_uint<32>&               drop_count_price_high,
    ap_uint<32>&               drop_count_below_base,
    ap_uint<32>&               drop_count_qty
) {
#pragma HLS PIPELINE II=1
    ap_uint<288> raw = in.read();

    ap_uint<32> price = raw.range( 31,   0);   // payload bytes 32-35
    ap_uint<32> qty   = raw.range(127,  96);   // payload bytes 20-23

    // Evaluate all conditions combinatorially — no early returns.
    // Mutual exclusivity is preserved by priority: price_high checked first,
    // then below_base, then qty. At most one fires per message.
    bool drop_high  = (price > filt_max);
    bool drop_low   = !drop_high && (price < filt_base);
    bool drop_qty   = !drop_high && !drop_low && (filt_minqty > 0) && (qty < filt_minqty);
    bool drop       = drop_high || drop_low || drop_qty;

    // Single counter update — HLS sees a mux, not three sequential RMWs.
    if (drop_high) ++drop_count_price_high;
    else if (drop_low)  ++drop_count_below_base;
    else if (drop_qty)  ++drop_count_qty;

    if (!drop) out.write(raw);
}

// parse_add_event: fan-out stage. Reads admitted payload, writes both streams.
void parse_add_event(
    hls::stream<ap_uint<288>>& in,
    hls::stream<MarketEvent>&  to_snapshot,
    hls::stream<MarketEvent>&  to_book
) {
#pragma HLS PIPELINE II=1
    ap_uint<288> raw = in.read();

    MarketEvent ev;
    ev.side  = raw.range(135, 128);  // payload byte 19
    ev.qty   = raw.range(127,  96);  // payload bytes 20-23
    ev.price = raw.range( 31,   0);  // payload bytes 32-35
    to_snapshot.write(ev);
    to_book.write(ev);
}

// HOT PATH -- simple running best bid/ask, II=1.
void update_snapshot(
    hls::stream<MarketEvent>& in,
    BookSnapshot&             snap
) {
#pragma HLS PIPELINE II=1
    MarketEvent ev = in.read();
    if (ev.side == 0x42 && ev.price > snap.bid_price) {  // 'B'
        snap.bid_price = ev.price;
        snap.bid_qty   = ev.qty;
    } else if (ev.side != 0x42 && ev.price < snap.ask_price) {
        snap.ask_price = ev.price;
        snap.ask_qty   = ev.qty;
    }
}

// COLD PATH -- direct BRAM read-modify-write for full order book depth.
// Always reads; gates BRAM write on ev.valid.
void register_book_update(
    hls::stream<MarketEvent>& in,
    const ap_uint<16>         inv_tick,
    const ap_uint<16>         base_offset
) {
    #pragma HLS PIPELINE II=1

    static Level levels[MAX_LEVELS * 2];
    #pragma HLS BIND_STORAGE variable=levels type=ram_2p impl=bram
    #pragma HLS DEPENDENCE variable=levels inter false

    MarketEvent ev = in.read();
    ap_uint<1>  side_bit = (ev.side == 0x42) ? ap_uint<1>(0) : ap_uint<1>(1);
    ap_uint<16> idx      = (ap_uint<16>)((ev.price * inv_tick) >> 16) - base_offset;
    uint16_t    addr     = (side_bit << BOOK_ADDR_WIDTH) | (uint16_t)idx;
    levels[addr].price = ev.price;
    levels[addr].qty  += ev.qty;
}

void kernel(
    hls::stream<ap_uint<352>>& feed_a,
    hls::stream<ap_uint<352>>& feed_b,
    BookSnapshot&              snap,
    const ap_uint<16>          inv_tick,
    const ap_uint<16>          base_offset,
    const ap_uint<64>          init_seq,
    const ap_uint<32>          filt_base,
    const ap_uint<32>          filt_max,
    const ap_uint<32>          filt_minqty
) {
    #pragma HLS INTERFACE ap_fifo   port=feed_a
    #pragma HLS INTERFACE ap_fifo   port=feed_b
    #pragma HLS INTERFACE ap_memory port=snap  depth=1
    #pragma HLS INTERFACE s_axilite port=inv_tick
    #pragma HLS INTERFACE s_axilite port=base_offset
    #pragma HLS INTERFACE s_axilite port=init_seq
    #pragma HLS INTERFACE s_axilite port=filt_base
    #pragma HLS INTERFACE s_axilite port=filt_max
    #pragma HLS INTERFACE s_axilite port=filt_minqty
    #pragma HLS DATAFLOW

    hls::stream<ap_uint<288>> arb_out;
    hls::stream<ap_uint<288>> filt_out;
    hls::stream<MarketEvent>  to_snapshot;
    hls::stream<MarketEvent>  to_book;
    #pragma HLS STREAM variable=to_book depth=512

    static ap_uint<32> drop_count_price_high = 0;
    static ap_uint<32> drop_count_below_base = 0;
    static ap_uint<32> drop_count_qty        = 0;

    const ap_uint<16> inv_tick_reg    = inv_tick;
    const ap_uint<16> base_offset_reg = base_offset;

    arbitrate(feed_a, feed_b, arb_out);
    filter_event(arb_out, filt_out, filt_base, filt_max, filt_minqty,
                 drop_count_price_high, drop_count_below_base, drop_count_qty);
    parse_add_event(filt_out, to_snapshot, to_book);
    update_snapshot(to_snapshot, snap);
    register_book_update(to_book, inv_tick_reg, base_offset_reg);
}
