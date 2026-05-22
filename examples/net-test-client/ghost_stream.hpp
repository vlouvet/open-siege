// Track 20 spec 10 — ghost-stream framing parser (clean-room).
//
// Decode the outer framing of a post-Connected Tribes 1.41 DataPacket
// to recover ghost-update records. Per the clean-room spec
// docs/clean-room-specs/TRIBES-NETPROTO.md §5.0, the format is:
//
//   * 4-byte VC header (variable: 1-bit flag, 1-bit parity, 9-bit
//     send-seq, 5-bit highest-recv, run-length ack list of 8-bit
//     entries terminated by a 3-bit zero count, 5-bit packet type).
//   * 1-bit "current-rate-changed" flag, optionally followed by
//     20 bits of update-delay+packet-size.
//   * 1-bit "max-rate-changed" flag, optionally followed by 20 bits.
//   * Per-sub-stream presence prefix: event-sub-stream, then
//     input/control-sub-stream, then ghost-sub-stream. Each is a
//     single 1-bit "present" flag followed by sub-stream contents
//     iff the bit is 1.
//   * Ghost-sub-stream body: 1-bit mode (1 = scope-always,
//     0 = normal), 3-bit width selector (encodes width − 3), then
//     a per-object loop terminating on a 0-valued object-present
//     bit (plus 1 more "scope-always done" bit in scope-always mode).
//   * Per-object record: 1-bit object-present, idW-bit ghost id,
//     1-bit kill flag, [scope-always-with-new-id only:] 32-bit
//     object id + 10-bit class tag, then a per-class payload of
//     opaque width (per spec 13, not yet decoded here).
//
// Open spec gaps (per spec §5.0.7):
//   1. The input/control sub-stream's bit length is not decoded by
//      this spec, so the start of the ghost-sub-stream within an
//      s→c DataPacket cannot be computed; it must be scan-found.
//   2. Without per-class payload widths (deferred to spec 13), the
//      parser cannot consume past the first record's class-tag
//      field; only the first record per packet is reported.
//
// Bit ordering is LSB-first within each byte. Multi-bit unsigned
// integers of width W read at bit position N are
//   sum over i in 0..W-1 of bit(N+i) * (1 << i)
// where bit(N) = (byte[N>>3] >> (N & 7)) & 1.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace net20 {

struct GhostUpdate {
    std::uint16_t ghost_id = 0;
    bool full_update = false;   // true if scope-always-mode "new id" payload was present
    bool kill = false;          // server is destroying this ghost
    bool scope_always_mode = false;
    std::uint8_t id_width_bits = 0;   // 3..10
    std::uint32_t object_id = 0;      // valid iff full_update
    std::uint16_t class_tag = 0;      // valid iff full_update, in 1..1023
    std::size_t start_bit = 0;        // bit offset where this record began
    std::vector<std::uint8_t> payload_bits;  // empty in v1 (per-class layout deferred)
};

struct GhostPacketDecode {
    // VC header decoded fields:
    bool vc_present = false;
    bool connect_parity = false;
    std::uint16_t send_seq = 0;           // 0..511
    std::uint8_t highest_recv = 0;        // 0..31
    std::uint8_t packet_type = 0;         // 0 = DataPacket
    std::size_t header_bits = 0;          // total VC-header bit length

    // Rate-control prefix decoded:
    bool rate_changed = false;
    std::uint16_t rate_update_delay_ms = 0;
    std::uint16_t rate_packet_size = 0;
    bool max_rate_changed = false;
    std::uint16_t max_update_delay_ms = 0;
    std::uint16_t max_packet_size = 0;
    std::size_t rate_prefix_end_bit = 0;

    // The bit offset within the packet at which the ghost-update sub-stream
    // body begins (i.e. immediately after the ghost-sub-stream-present bit
    // was read as 1). std::nullopt if no plausible ghost stream was found.
    std::optional<std::size_t> ghost_stream_start_bit;

    // First record per packet (per-class payload widths are not yet decoded,
    // so only the leading record is recoverable without spec 13).
    std::vector<GhostUpdate> updates;

    // Diagnostic notes for callers.
    std::string note;
};

// Parse a single UDP datagram as a Tribes 1.41 VC DataPacket and decode the
// leading record of any ghost-sub-stream it contains. Returns a structure
// even on framing errors; the `note` field describes what went wrong.
GhostPacketDecode parse_ghost_packet(const std::uint8_t* data,
                                     std::size_t length);

// Convenience: format one record as a single log line.
std::string format_update(const GhostUpdate& u);

}  // namespace net20
