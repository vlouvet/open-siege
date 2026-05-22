// Track 20 spec 22 — client-ready event encoder (clean-room).
// See client_events.hpp for the wire reference (clean-room §16.5).

#include "client_events.hpp"

namespace net20 {

namespace {

// Pad the BitWriter cursor up to the next byte boundary by writing
// zero bits. Used to align the uncompressed-string body per §16.4.
void byte_align(BitWriter& w)
{
    const std::size_t mod = w.pos() & 7u;
    if (mod == 0) return;
    w.write_bits(0u, static_cast<unsigned>(8u - mod));
}

}  // namespace

std::vector<std::uint8_t> encode_client_ready(const ClientReadyInputs& inputs)
{
    BitWriter w;

    // ----- VC header (§14.2 / §3.1) -----
    w.write_flag(true);                     // bit 0: VC discriminator
    w.write_flag(inputs.connect_parity);    // bit 1: connect-handle parity
    w.write_bits(inputs.send_seq & 0x1FFu, 9);                 // bits 2..10
    w.write_bits(inputs.highest_acked_of_mine & 0x1Fu, 5);     // bits 11..15

    // Ack run-length list (§14.3): 8 bits per element (3-bit length
    // 1..7 + 5-bit start_seq mod 32).
    for (const AckRun& r : inputs.ack_runs) {
        const std::uint8_t len = r.length == 0 ? 1
            : (r.length > 7 ? 7 : r.length);
        w.write_bits(len, 3);
        w.write_bits(r.start_seq & 0x1Fu, 5);
    }
    // Terminator: 3-bit zero count + 5-bit type word (= 0 for DataPacket).
    w.write_bits(0u, 3);
    w.write_bits(static_cast<std::uint32_t>(pkt_type::kDataPacket) & 0x1Fu, 5);

    // ----- Rate-control prefix (§3.4) -----
    // R0 = 1 with current update-delay + packet-size.
    w.write_flag(true);
    w.write_bits(inputs.cur_update_delay_ms & 0x3FFu, 10);
    w.write_bits(inputs.cur_packet_size_bytes & 0x3FFu, 10);
    // R1 = 1 with max update-delay + max packet-size.
    w.write_flag(true);
    w.write_bits(inputs.max_update_delay_ms & 0x3FFu, 10);
    w.write_bits(inputs.max_packet_size_bytes & 0x3FFu, 10);

    // ----- Event sub-stream (§16.1/§16.2) -----
    w.write_flag(true);    // event-sub-stream-present

    // Event #1: guaranteed-ordered remote-console-dispatch, seq=0.
    w.write_flag(true);    // event-present
    w.write_flag(true);    // guaranteed
    w.write_flag(false);   // seq-continuous = 0 (first guaranteed event)
    w.write_flag(true);    // has-explicit-seq
    w.write_bits(0u, 7);   // explicit ordered seq = 0
    w.write_bits(8u, 7);   // class id wire value = 8 (= real tag 1032)

    // ----- Per-event payload (§16.4) -----
    w.write_bits(1u, 5);   // argc = 1

    // String #1: uncompressed 1-byte body.
    w.write_flag(false);   // compression flag = 0 (uncompressed)
    w.write_bits(1u, 8);   // byte length = 1
    // Uncompressed body is byte-aligned per §16.4.
    byte_align(w);
    w.write_bits(inputs.arg_byte, 8);

    // Event sub-stream terminator.
    w.write_flag(false);   // event-present = 0

    // ----- Input/control sub-stream present (§5.0.2) -----
    w.write_flag(false);   // we have no input frame to send

    // ----- Ghost sub-stream present (§5.0.2) -----
    w.write_flag(false);   // client never authoritatively ghosts

    // Trailing zero-pad to next byte boundary is implicit: the buffer is
    // sized to (bit_pos + 7) / 8 bytes, and any unused high bits in the
    // final byte are already zero (write_bits never sets them).

    return std::move(w.bytes);
}

// Captured 59-byte working client-ready packet from
// captures/real-tribes/groove-session-20260522-124329.json (packet i=4,
// the first c→s 59-byte DataPacket sent ≈210 ms after AcceptConnect in
// a session that successfully transitioned to gameplay).
//
// Bytes 0..3 are the VC header (overwritten per call); bytes 4..58 are
// the event sub-stream + input sub-stream payload that the server
// requires. We're replaying these verbatim because the function-name
// Huffman encoding and the input PSC payload are not yet decoded.
static const std::uint8_t kCapturedReadyBody[55] = {
    0x85, 0x80, 0xac, 0x10, 0x90, 0x5d, 0x00, 0x22,
    0x84, 0x88, 0xb6, 0x33, 0x20, 0xa6, 0x4e, 0x0c,
    0x2c, 0xbd, 0x57, 0xc3, 0x1f, 0xa1, 0x26, 0x4c,
    0x1f, 0xd6, 0x93, 0x8f, 0xa2, 0x4d, 0x13, 0xec,
    0xb2, 0x3d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
    0x20, 0xb3, 0x60, 0xcb, 0xd1, 0xbe, 0x40, 0x74,
    0x51, 0xaa, 0x00, 0x00, 0x00, 0x00, 0x0a,
};

std::vector<std::uint8_t> encode_client_ready_verbatim(const ClientReadyInputs& inputs)
{
    // Build the 4-byte VC header into a small BitWriter, then concat
    // the captured body. Per §14.2: bit 0 VC, bit 1 parity, bits 2..10
    // send-seq, bits 11..15 highest-acked-of-mine, then ack-run list
    // terminated by a 3-bit zero + 5-bit type=0 (DataPacket).
    BitWriter w;
    w.write_flag(true);
    w.write_flag(inputs.connect_parity);
    w.write_bits(inputs.send_seq & 0x1FFu, 9);
    w.write_bits(inputs.highest_acked_of_mine & 0x1Fu, 5);
    for (const AckRun& r : inputs.ack_runs) {
        const std::uint8_t len = r.length == 0 ? 1
            : (r.length > 7 ? 7 : r.length);
        w.write_bits(len, 3);
        w.write_bits(r.start_seq & 0x1Fu, 5);
    }
    w.write_bits(0u, 3);  // ack-list terminator
    w.write_bits(static_cast<std::uint32_t>(pkt_type::kDataPacket) & 0x1Fu, 5);

    // Header should be exactly 4 bytes for a VC packet with one ack-run.
    // If callers pass weird ack lists the header may grow; we still
    // append the body, but the resulting packet won't match the capture
    // shape and probably won't work.
    std::vector<std::uint8_t> out = std::move(w.bytes);
    out.insert(out.end(),
        std::begin(kCapturedReadyBody), std::end(kCapturedReadyBody));
    return out;
}

}  // namespace net20
