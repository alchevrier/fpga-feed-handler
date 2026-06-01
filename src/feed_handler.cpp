#include "feed_handler.hpp"

// msg is packed big-endian: byte 0 at bits [287:280], byte 35 at bits [7:0]
// byte k => msg.range(287 - 8*k, 280 - 8*k)
void parse_add_event(
    hls::stream<ap_uint<288>>& in,
    hls::stream<MarketEvent>& out,
    const ap_uint<16> inv_tick,
    const ap_uint<16> base_offset
) {
    #pragma HLS PIPELINE II=1
    ap_uint<288> msg = in.read();

    MarketEvent ev;
    ev.stock_locate = msg.range(279, 264);  // bytes  1-2
    ev.order_id     = msg.range(199, 136);  // bytes 11-18
    ev.side         = msg.range(135, 128);  // byte  19
    ev.qty          = msg.range(127,  96);  // bytes 20-23
    ev.price        = msg.range( 31,   0);  // bytes 32-35
    ev.idx          = (ap_uint<16>)((ev.price * inv_tick) >> 16) - base_offset;
    out.write(ev);
}

void handle_event(
    hls::stream<MarketEvent>& cur,
    hls::stream<LevelUpdate>& upd
) {

    static Level levels[MAX_LEVELS * 2];
    #pragma HLS BIND_STORAGE variable=levels type=ram_2p
  
    MarketEvent ev = cur.read();
    ap_uint<1> side_bit = (ev.side == 0x42) ? 0 : 1;  // bid=0, ask=1
    uint16_t addr = (side_bit << BOOK_ADDR_WIDTH) | (uint16_t)ev.idx;
    levels[addr].price = ev.price;
    levels[addr].qty += ev.qty;
    upd.write({ ev.price, ev.qty, ev.side });
}

void update_snapshot(
    BookSnapshot& snap,
    hls::stream<LevelUpdate>& levelUpdate
) {
    #pragma HLS PIPELINE II=1
    LevelUpdate upd = levelUpdate.read();
    if (upd.side == 0x42 && upd.price > snap.bid_price) {  // 'B'
        snap.bid_price = upd.price;
        snap.bid_qty   = upd.qty;
    } else if (upd.side != 0x42 && upd.price < snap.ask_price) {
        snap.ask_price = upd.price;
        snap.ask_qty   = upd.qty;
    }
}

void kernel(
  hls::stream<ap_uint<288>>& in,
  BookSnapshot&              snap,
  const ap_uint<16>          inv_tick,
  const ap_uint<16>          base_offset
) {
  #pragma HLS INTERFACE ap_fifo   port=in
  #pragma HLS INTERFACE ap_memory port=snap
  #pragma HLS INTERFACE s_axilite port=inv_tick
  #pragma HLS INTERFACE s_axilite port=base_offset
  #pragma HLS DATAFLOW

  hls::stream<MarketEvent> event;
  hls::stream<LevelUpdate> levelUpdate;

  const ap_uint<16> inv_tick_reg    = inv_tick;
  const ap_uint<16> base_offset_reg = base_offset;

  parse_add_event(in, event, inv_tick_reg, base_offset_reg);
  handle_event(event, levelUpdate);
  update_snapshot(snap, levelUpdate);
}