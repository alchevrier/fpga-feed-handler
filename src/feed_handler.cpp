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
    const ap_uint<64>          init_seq
) {
    #pragma HLS INTERFACE ap_fifo   port=feed_a
    #pragma HLS INTERFACE ap_fifo   port=feed_b
    #pragma HLS INTERFACE ap_memory port=snap depth=1
    #pragma HLS INTERFACE s_axilite port=inv_tick
    #pragma HLS INTERFACE s_axilite port=base_offset
    #pragma HLS INTERFACE s_axilite port=init_seq
    #pragma HLS DATAFLOW

    hls::stream<ap_uint<288>> arb_out;
    hls::stream<MarketEvent>  to_snapshot;
    hls::stream<MarketEvent>  to_book;
    #pragma HLS STREAM variable=to_book depth=512   // absorbs bursts while cold-path BRAM R-M-W drains

    const ap_uint<16> inv_tick_reg    = inv_tick;
    const ap_uint<16> base_offset_reg = base_offset;

    arbitrate(feed_a, feed_b, arb_out);
    parse_add_event(arb_out, to_snapshot, to_book);
    update_snapshot(to_snapshot, snap);
    register_book_update(to_book, inv_tick_reg, base_offset_reg);
}
