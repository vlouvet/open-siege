// Track 20 spec 12 — reliable-channel ack encoder (clean-room).
//
// Implements the VC ack-list emit / track logic specified in
// docs/clean-room-specs/TRIBES-NETPROTO.md §14:
//
//   * 32-entry "to be acked" window indexed by incoming send-seq mod 32.
//     Set the slot true when a non-Ping VC datagram arrives.
//   * On every outgoing datagram, scan the window for runs of contiguous
//     true slots starting from the lowest start point and emit each as an
//     8-bit element (3-bit length 1..7, 5-bit start seq mod 32).
//     A run longer than 7 is split.
//   * After the last real element, emit the 3-bit zero terminator + 5-bit
//     packet-type / flag word (type 16 = "pure ack", type 0 = "DataPacket",
//     etc. — see §14.4).
//   * Header preceding the ack list:
//       bit 0       — VC discriminator (always 1 for in-band VC traffic)
//       bit 1       — connect-handle parity (LSB of agreed handle)
//       bits 2..10  — 9-bit send sequence (wraps mod 512); pure-ack and
//                     pure-ping do NOT increment the counter (§14.2)
//       bits 11..15 — 5-bit highest-acked-of-mine (mod 32)
//
//   * Cadence (§14.5): if 12 or more window slots are in the "to-be-acked"
//     state, an explicit pure-ack must be emitted immediately. Otherwise
//     the next outgoing data-bearing datagram naturally carries the acks.
//
// Bit ordering is LSB-first within each byte: bit N of the stream lives at
//   (byte[N >> 3] >> (N & 7)) & 1
// (matches the convention spec §14.1 and the ghost_stream parser already
// implement.)
//
// Test vector (§14.7): a pure-ack acknowledging server seq 1, with our
// own send-seq = 1 and highest-acked-of-mine = 2, connect parity 0:
//
//   bytes = { 0x05, 0x08, 0x09, 0x80 }
//
// `encode_pure_ack({1, 0, 2, {1}})` returns exactly that.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace net20 {

// Bitwise writer over a growable byte buffer, LSB-first within byte.
struct BitWriter {
    std::vector<std::uint8_t> bytes;
    std::size_t bit_pos = 0;

    void write_bits(std::uint32_t value, unsigned width);
    void write_flag(bool b) { write_bits(b ? 1u : 0u, 1); }
    // Current bit cursor.
    std::size_t pos() const noexcept { return bit_pos; }
};

// Bitwise reader, LSB-first within byte (mirror of BitWriter). Used by
// the movecommand decoder (spec 28/02) and any future VC-stream parser.
// Reads past end-of-buffer set `overflow = true` and return 0.
struct BitReader {
    const std::uint8_t* data = nullptr;
    std::size_t         size = 0;       // capacity in bytes
    std::size_t         bit_pos = 0;
    bool                overflow = false;

    BitReader() = default;
    BitReader(const std::uint8_t* d, std::size_t n) : data(d), size(n) {}

    std::uint32_t read_bits(unsigned width);
    bool          read_flag() { return read_bits(1) != 0; }
    // Read a 32-bit IEEE-754 float written by BitWriter::write_bits(32).
    // Returns 0.0f on overflow.
    float         read_float32_le();
    // True if any read attempted past size_bytes*8.
    bool          fail() const noexcept { return overflow; }
    std::size_t   pos() const noexcept { return bit_pos; }
};

// Packet-type / flag word values (§14.4).
namespace pkt_type {
constexpr std::uint8_t kDataPacket     = 0;
constexpr std::uint8_t kDisconnect     = 2;
constexpr std::uint8_t kRequestConnect = 3;
constexpr std::uint8_t kAcceptConnect  = 4;
constexpr std::uint8_t kRejectConnect  = 5;
constexpr std::uint8_t kPing           = 7;
constexpr std::uint8_t kResendFlag     = 0x08;   // OR onto base type
constexpr std::uint8_t kAckFlag        = 0x10;   // OR onto base type
// Convenience: "pure ack" type word value (= 16).
constexpr std::uint8_t kPureAck        = kAckFlag;
}  // namespace pkt_type

// Track received server send-seqs so we can ack them back. The window is a
// rolling 32-slot boolean array indexed by (received_seq mod 32).
struct AckTracker {
    // True iff slot s mod 32 has been received but not yet acked.
    std::array<bool, 32> received{};
    // Highest received seq observed (mod 32). Reported back to the peer as
    // bits 11..15 of the VC header.
    std::uint8_t highest_recv_mod32 = 0;
    // Total count of received non-Ping datagrams ever (for diagnostics).
    std::uint64_t total_received = 0;
    // Total acks emitted (diagnostic).
    std::uint64_t total_acks_sent = 0;
    // Last time, in steady_clock ms, that we emitted a datagram (for the
    // 32-ms min-interval check in §14.5 rule 4).
    std::uint64_t last_emit_ms = 0;

    // Mark a received server datagram for ack. Called from the recv path
    // once the incoming send-seq has been parsed out of the VC header.
    void on_receive(std::uint16_t server_send_seq);

    // Clear all "to be acked" slots — caller invokes this immediately after
    // emit_ack_list has written them onto the wire.
    void clear_pending();

    // Count currently-pending acks. Used to trigger rule-3 force emission
    // (§14.5: 12 or more pending → must emit a pure-ack now).
    std::size_t pending_count() const noexcept;

    // Return true if rule-3 demands a force ack right now.
    bool should_force_ack() const noexcept { return pending_count() >= 12; }
};

// Build the run-length list (§14.3) from a window of "to be acked" slots,
// using `highest_recv_mod32` as the starting walk point. Runs longer than 7
// are split into multiple length-7 elements. Output: a list of (length,
// start_seq) tuples in emission order, where each length is in 1..7.
struct AckRun {
    std::uint8_t length;     // 1..7
    std::uint8_t start_seq;  // 0..31
};
std::vector<AckRun> build_ack_runs(const std::array<bool, 32>& window,
                                   std::uint8_t highest_recv_mod32);

// Encode an entire VC datagram header (no body), suitable for a pure-ack
// emit. `type_word` is one of pkt_type::* (commonly kPureAck = 16).
struct VcHeaderInputs {
    std::uint16_t send_seq;          // 0..511; reused for pure-ack
    bool connect_parity;             // bit 1
    std::uint8_t highest_acked_of_mine; // bits 11..15, mod 32
    std::vector<AckRun> ack_runs;    // from build_ack_runs
    std::uint8_t type_word;          // bits at end of list (5 bits; e.g. 16)
};
std::vector<std::uint8_t> encode_vc_header(const VcHeaderInputs& inputs);

// Convenience: encode a pure-ack datagram given the four pieces of state.
struct PureAckInputs {
    std::uint16_t send_seq;
    bool connect_parity;
    std::uint8_t highest_acked_of_mine;
    std::vector<std::uint8_t> ack_runs_starts;  // start_seq mod 32 for each
                                                 // contiguous run; lengths
                                                 // inferred from contiguity
};
// Lightweight helper used by the unit-test path; lengths are auto-derived
// from the contiguity of the input vector (which must be sorted ascending
// within a single contiguous run). For multi-run encoding use the full
// `encode_vc_header` path.
std::vector<std::uint8_t> encode_pure_ack_simple(std::uint16_t send_seq,
                                                 bool connect_parity,
                                                 std::uint8_t highest_acked,
                                                 std::uint8_t run_length,
                                                 std::uint8_t run_start);

// Parse the incoming server send-seq out of a VC datagram. Returns
// std::nullopt if the packet is too short or doesn't look like a VC
// datagram. Also reports the decoded packet-type word so the caller can
// skip Ping (type 7) per §14.5 rule 1.
struct ParsedIncomingHeader {
    std::uint16_t send_seq;       // 0..511
    std::uint8_t  type_word;      // raw 5-bit value (may have Resend|Ack flags)
    std::uint8_t  base_type;      // type_word & 0x07
    bool          is_ack;         // (type_word & 0x10) != 0
    bool          is_resend;      // (type_word & 0x08) != 0
    // 14c-I-osgb-gate — bits 11..15 of the incoming header: the peer's
    // "highest acked of mine (mod 32)". Used by the listener to
    // determine when the peer has acknowledged the initial burst.
    std::uint8_t  peer_highest_acked_of_ours_mod32;
};
// Returns true on success. Mirrors the ghost_stream decoder's VC header walk
// but exposes the parsed seq + type to the caller (the ghost stream parser
// keeps them internal).
bool parse_incoming_header(const std::uint8_t* data, std::size_t length,
                           ParsedIncomingHeader& out);

}  // namespace net20
