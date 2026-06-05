#include "feed_handler.hpp"
#include <cstdio>

// ASML Add Order messages from 01302019.NASDAQ_ITCH50
//
// Instrument parameters (host-derived, written via s_axilite before kernel start):
//   base_price = 1749700  — lowest observed ASML price in this dataset, multiple of 100
//   tick_size  = 100      — NMS stock >= $1.00 rule
//   inv_tick   = (1 << 16) / 100 = 655

// Host-side precomputation — written once to s_axilite before feed starts
static const ap_uint<16> INV_TICK    = 655;   // (1 << 16) / tick_size = 65536 / 100
static const ap_uint<16> BASE_OFFSET = (ap_uint<16>)((1749700u * 655u) >> 16);  // = 17496
static const ap_uint<64> INIT_SEQ    = 1;     // MOLDUDP64 sessions start at seq=1, never 0

// Filter configuration scalars — passed individually as s_axilite to avoid ap_memory
// read latency (struct ap_memory costs 1 cycle per field; scalars are combinatorial wires).
//   filt_base   = 1749700 ($174.97) — lowest observed ASML price in dataset
//   filt_max    = 1949700 ($194.97) — precomputed: base_price + 2000*100
//   filt_minqty = 0 (disabled)
static const ap_uint<32> FILT_BASE   = 1749700;
static const ap_uint<32> FILT_MAX    = 1949700;
static const ap_uint<32> FILT_MINQTY = 0;

// msg 2: side=B  qty=500   price=1758400 ($175.84)
static const uint8_t asml_b_175_84[36] = {
    0x41, 0x02, 0x19, 0x00, 0x00, 0x0D, 0x18, 0xC2, 0xF3, 0x63, 0xD6, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x1D, 0x42, 0x00, 0x00, 0x01, 0xF4,
    0x41, 0x53, 0x4D, 0x4C, 0x20, 0x20, 0x20, 0x20, 0x00, 0x1A, 0xD4, 0xC0
};
// msg 3: side=B  qty=500   price=1758000 ($175.80)
static const uint8_t asml_b_175_80[36] = {
    0x41, 0x02, 0x19, 0x00, 0x00, 0x0D, 0x18, 0xC2, 0xF5, 0x3F, 0xD7, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x49, 0x42, 0x00, 0x00, 0x01, 0xF4,
    0x41, 0x53, 0x4D, 0x4C, 0x20, 0x20, 0x20, 0x20, 0x00, 0x1A, 0xD3, 0x30
};
// msg 4: side=B  qty=400   price=1757700 ($175.77)
static const uint8_t asml_b_175_77[36] = {
    0x41, 0x02, 0x19, 0x00, 0x00, 0x0D, 0x18, 0xC2, 0xF7, 0x13, 0xA8, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x81, 0x42, 0x00, 0x00, 0x01, 0x90,
    0x41, 0x53, 0x4D, 0x4C, 0x20, 0x20, 0x20, 0x20, 0x00, 0x1A, 0xD2, 0x04
};
// msg 5: side=B  qty=1500  price=1749700 ($174.97)  — at base_price, idx=0
static const uint8_t asml_b_174_97[36] = {
    0x41, 0x02, 0x19, 0x00, 0x00, 0x0D, 0x18, 0xC2, 0xF8, 0xC8, 0xD9, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0xB5, 0x42, 0x00, 0x00, 0x05, 0xDC,
    0x41, 0x53, 0x4D, 0x4C, 0x20, 0x20, 0x20, 0x20, 0x00, 0x1A, 0xB2, 0xC4
};
// msg 6: side=S  qty=500   price=1761200 ($176.12)
static const uint8_t asml_s_176_12[36] = {
    0x41, 0x02, 0x19, 0x00, 0x00, 0x0D, 0x18, 0xC2, 0xFA, 0x24, 0x41, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0xDD, 0x53, 0x00, 0x00, 0x01, 0xF4,
    0x41, 0x53, 0x4D, 0x4C, 0x20, 0x20, 0x20, 0x20, 0x00, 0x1A, 0xDF, 0xB0
};
// msg 7: side=S  qty=500   price=1761500 ($176.15)
static const uint8_t asml_s_176_15[36] = {
    0x41, 0x02, 0x19, 0x00, 0x00, 0x0D, 0x18, 0xC2, 0xFB, 0xDB, 0xE8, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x25, 0x11, 0x53, 0x00, 0x00, 0x01, 0xF4,
    0x41, 0x53, 0x4D, 0x4C, 0x20, 0x20, 0x20, 0x20, 0x00, 0x1A, 0xE0, 0xDC
};
// msg 8: side=S  qty=500   price=1761700 ($176.17)
static const uint8_t asml_s_176_17[36] = {
    0x41, 0x02, 0x19, 0x00, 0x00, 0x0D, 0x18, 0xC2, 0xFE, 0x07, 0x7C, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x25, 0x45, 0x53, 0x00, 0x00, 0x01, 0xF4,
    0x41, 0x53, 0x4D, 0x4C, 0x20, 0x20, 0x20, 0x20, 0x00, 0x1A, 0xE1, 0xA4
};

static const uint8_t* msgs[7] = {
    asml_b_175_84, asml_b_175_80, asml_b_175_77, asml_b_174_97,
    asml_s_176_12, asml_s_176_15, asml_s_176_17
};

static ap_uint<352> pack_msg(const uint8_t* msg, ap_uint<64> seq) {
    // bits [351:288] = MOLDUDP64 sequence number (prepended by host DMA)
    // bits [287:0]   = ITCH payload, byte 0 at bits [287:280]
    ap_uint<288> payload = 0;
    for (int i = 0; i < 36; i++)
        payload = (payload << 8) | msg[i];
    return (ap_uint<352>(seq) << 288) | ap_uint<352>(payload);
}

int main() {
    // snap persists across kernel calls — models the hardware register state
    BookSnapshot snap = {0, 0, 0xFFFFFFFF, 0};
    int pass = 1;

    // All 7 messages routed through feed_a (primary feed).
    // feed_b is empty each invocation — models secondary feed idle.
    // arbitrate() seeds expected_seq from INIT_SEQ on the first call (static seeded flag),
    // then owns the counter for all subsequent calls without re-seeding.
    for (int m = 0; m < 7; m++) {
        hls::stream<ap_uint<352>> feed_a;
        hls::stream<ap_uint<352>> feed_b;  // empty — secondary feed idle this cycle
        feed_a.write(pack_msg(msgs[m], INIT_SEQ + m));  // seq 1..7
        kernel(feed_a, feed_b, snap, INV_TICK, BASE_OFFSET, INIT_SEQ, FILT_BASE, FILT_MAX, FILT_MINQTY);
    }

    // Expected final snapshot after all 7 ASML messages:
    //   Best bid = highest bid price seen = $175.84 = 1758400, qty=500  (msg 2)
    //   Best ask = lowest ask price seen  = $176.12 = 1761200, qty=500  (msg 6)
    //
    // Bid msgs 3/4/5 are all lower than msg 2 — snapshot not updated.
    // Ask msgs 7/8 are higher than msg 6 — snapshot not updated.

    if (snap.bid_price != 1758400) {
        printf("FAIL bid_price: got %u expected 1758400\n", (unsigned)snap.bid_price);
        pass = 0;
    }
    if (snap.bid_qty != 500) {
        printf("FAIL bid_qty:   got %u expected 500\n", (unsigned)snap.bid_qty);
        pass = 0;
    }
    if (snap.ask_price != 1761200) {
        printf("FAIL ask_price: got %u expected 1761200\n", (unsigned)snap.ask_price);
        pass = 0;
    }
    if (snap.ask_qty != 500) {
        printf("FAIL ask_qty:   got %u expected 500\n", (unsigned)snap.ask_qty);
        pass = 0;
    }

    printf("%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
