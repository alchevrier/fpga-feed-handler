#pragma once
#include <ap_int.h>
#include <hls_stream.h>

static constexpr int MAX_LEVELS = 128;
static constexpr int BOOK_ADDR_WIDTH = 7;  // log2(MAX_LEVELS)

struct Level {
    ap_uint<32> price;
    ap_uint<32> qty;
};

// Extracted from a single ITCH Add Order message
struct MarketEvent {
    ap_uint<32> price;
    ap_uint<32> qty;
    ap_uint<64> order_id;
    ap_uint<16> stock_locate;
    ap_uint<16> idx;       // precomputed BRAM index from parse stage
    ap_uint<8>  side;      // 'B' = 0x42, 'S' = 0x53
};

// What handle_event tells update_snapshot after a BRAM write
struct LevelUpdate {
    ap_uint<32> price;
    ap_uint<32> qty;
    ap_uint<8>  side;          // which side was updated
};

// Best bid/ask snapshot registers (UltraFast flip-flops, not BRAM)
struct BookSnapshot {
    ap_uint<32> bid_price;
    ap_uint<32> bid_qty;
    ap_uint<32> ask_price;
    ap_uint<32> ask_qty;
};

// Top-level kernel declaration (defined in feed_handler.cpp)
// feed_a / feed_b: ap_uint<352> — bits [351:288] = MOLDUDP64 seq, bits [287:0] = ITCH payload
// inv_tick    = 65536 / tick_size                    — fits in ap_uint<16>
// base_offset = (base_price * inv_tick) >> 16        — precomputed by host at config time
// init_seq    = starting MOLDUDP64 sequence number   — 1 at session open, arbitrary on gap recovery
// idx is computed in parse_add_event (pipelined II=1) — multiply absorbed into pipeline
void kernel(hls::stream<ap_uint<352>>& feed_a,
  hls::stream<ap_uint<352>>& feed_b,
  BookSnapshot&              snap,
  const ap_uint<16>          inv_tick,
  const ap_uint<16>          base_offset,
  const ap_uint<64>          init_seq
);