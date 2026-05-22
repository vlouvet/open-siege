// Track 20 spec 22 — client-ready event encoder (clean-room).
//
// Build the c→s `DataPacket` that carries the connection-progression
// reliable event described in §16.5 of
// docs/clean-room-specs/TRIBES-NETPROTO.md. After the client has
// completed the handshake (RequestConnect → AcceptConnect → pure-ack
// of the AcceptConnect) the server stays stuck retransmitting its
// first ghost burst until it receives a guaranteed reliable event on
// the event sub-stream. Per §16.5 step 2, the act of receiving any
// guaranteed event (regardless of dispatch result) is what unblocks
// the server's state machine; the contents of the function-name
// string do not matter — even a single ASCII byte (`'A'`) works.
//
// This encoder builds that minimal packet:
//   * VC header (§14.2/§3.1) with type_word = 0 (DataPacket),
//     ack-run list copied from the supplied AckTracker.
//   * Rate-control prefix (§3.4): R0 = 1 with delay/packsize, R1 = 1
//     with max-delay/max-packsize. The Groove client advertises 66 ms
//     / 400 bytes for both; we match.
//   * Event sub-stream (§16.2): one event, guaranteed, has-explicit-seq
//     with seq = 0, class-id wire value = 8 (= real tag 1032,
//     remote-console dispatch).
//   * Event payload (§16.4): argc = 1, one uncompressed
//     1-byte string body (compression-flag = 0, length = 1, ASCII
//     'A'). The uncompressed body is byte-aligned per §16.4.
//   * Input-sub-stream-present = 0, ghost-sub-stream-present = 0.
//   * Trailing zero pad to byte boundary.
//
// Bit ordering is LSB-first within each byte (matches reliable_acks.*).

#pragma once

#include "reliable_acks.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace net20 {

// Inputs for the spec 20/22 client-ready DataPacket emit.
struct ClientReadyInputs {
    std::uint16_t send_seq;              // 0..511; per §14.2 a non-pure-ack
                                          // DataPacket increments the counter.
    bool connect_parity;                  // bit 1 of the VC header
    std::uint8_t highest_acked_of_mine;  // bits 11..15 (mod 32)
    std::vector<AckRun> ack_runs;        // run-length acks for §14.3

    // Rate-control fields (§3.4). Hard-coded values match the Groove
    // capture (66 ms / 400 B); the spec confirms anything within the
    // §3.4 clamp range is acceptable.
    std::uint16_t cur_update_delay_ms   = 66;
    std::uint16_t cur_packet_size_bytes = 400;
    std::uint16_t max_update_delay_ms   = 66;
    std::uint16_t max_packet_size_bytes = 400;

    // The one-character string argument. §16.5 step 2 says any
    // function-name string fires the same server state transition
    // because the server's reaction is driven by the receipt of a
    // guaranteed event, not by the dispatch result. We default to a
    // single 'A' character.
    std::uint8_t arg_byte = 'A';
};

// Encode the spec 20/22 client-ready packet. Returns the wire bytes.
std::vector<std::uint8_t> encode_client_ready(const ClientReadyInputs& inputs);

// Spec 20/23 follow-up — the "any guaranteed event unblocks" theory in
// §16.5 turned out to be wrong empirically; the server kept resending
// AcceptConnect after our 1-byte-'A' ready. As a fallback that doesn't
// require fully reverse-engineering the Huffman table + input
// sub-stream, this encoder takes the body bytes from a captured
// working Groove session (the 55 bytes after the 4-byte VC header of
// `groove-session-20260522-124329.json` packet i=4) and prepends a
// fresh 4-byte VC header built from `inputs`. Sends 59 bytes total.
std::vector<std::uint8_t> encode_client_ready_verbatim(const ClientReadyInputs& inputs);

}  // namespace net20
