#ifndef OSENGINE_TAH_VC_OUTBOUND_HPP
#define OSENGINE_TAH_VC_OUTBOUND_HPP

// Spec 26/14c — per-session outbound VC packet builder (clean-room).
//
// Implements the build path documented in
// docs/clean-room-specs/TRIBES-VC-OUTBOUND.md. The receive-side primitives
// (BitWriter, AckTracker, build_ack_runs, encode_vc_header,
// parse_incoming_header) already live in reliable_acks.hpp / .cpp. This
// header layers a Session-aware façade on top that:
//
//   1. Applies the send-seq bump rule from §4.1 of the spec (DataPacket
//      and the four connection-control types BUMP by 1 mod 512; Ping and
//      pure-Ack REUSE the previous value).
//   2. Composes the 5-bit type-word from the requested base type plus the
//      Resend (0x08) and Ack (0x10) flag bits.
//   3. Writes the 16-bit fixed header + ack-run list + terminator + type
//      word at the start of an outbound datagram, using the session's
//      live AckTracker + connect_handle parity + send_seq state.
//   4. For the four connection-control types (Disconnect, RequestConnect,
//      AcceptConnect, RejectConnect), bit-packs the session's full 32-bit
//      `connect_handle` immediately after the type word at the current
//      (possibly non-byte-aligned) bit cursor (§5.2, §8 step 6).
//
// Two API shapes are exposed:
//
//   * begin_outbound_packet(session, plan) -> OutboundPacket
//       Stateless. Returns a struct containing the partially-written
//       BitWriter plus the chosen send_seq. The caller appends body bits
//       and calls finalize_outbound_packet(...) to pad + commit.
//
//   * OutboundPacketBuilder { ... }
//       Stateful RAII wrapper that owns the BitWriter and tracks the
//       chosen send_seq, so the eventual finish() can commit (or NOT, for
//       non-bumping types) the bump back to the session.
//
// Either shape produces identical bytes. The selftest exercises both.

#pragma once

#include <osengine/reliable_acks.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dts_viewer
{

struct Session;

namespace tah_vc
{

// Plan for one outbound datagram. The caller fills this in, then calls
// begin_outbound_packet(...) which interprets the fields per the spec.
struct OutboundPacketPlan
{
    std::uint8_t base_type = net20::pkt_type::kDataPacket;  // 0,2,3,4,5,7
    bool         is_resend = false;     // OR in 0x08
    bool         is_ack    = false;     // OR in 0x10 (pure-ack convention)
    // For connection-control types, the body begins with the session's
    // 32-bit connect_handle bit-packed at the current cursor. Set this
    // to true for base_type in {Disconnect, RequestConnect, AcceptConnect,
    // RejectConnect}. begin_outbound_packet() applies the default
    // automatically based on base_type; this flag overrides if the caller
    // wants to suppress (it shouldn't, in practice).
    bool         emit_connect_handle = true;
};

// Result of begin_outbound_packet(): the in-progress BitWriter (caller
// appends body bits), plus the send_seq that was written into the header.
// The caller is responsible for calling finalize_outbound_packet(...) to
// pad to a byte boundary and update the session's send_seq counter and
// last_outbound_ms watermark.
struct OutboundPacket
{
    net20::BitWriter writer;
    std::uint16_t    written_send_seq = 0;
    std::uint8_t     type_word        = 0;
    bool             bumped_seq       = false;   // for finalize bookkeeping
};

// Apply the bump rule from spec §4.1. Returns true iff the requested type
// is one of the bumping types (DataPacket / Disconnect / RequestConnect /
// AcceptConnect / RejectConnect; any combination with flag bits also
// bumps for these base types). Ping (7) and pure-Ack (Ack flag with base
// 0) do NOT bump.
bool should_bump_send_seq(std::uint8_t base_type, bool is_ack_flag) noexcept;

// Compose a 5-bit type word from the base type + flag intents.
std::uint8_t compose_type_word(std::uint8_t base_type,
                               bool         is_resend,
                               bool         is_ack) noexcept;

// Build the VC header for one outbound datagram against `sess`. After
// return, the BitWriter's bit cursor sits at the next-free bit position
// immediately following the 5-bit type word (and, for connection-control
// types, immediately following the 32-bit connect_handle). The caller
// appends type-specific body bits and then calls
// finalize_outbound_packet(...).
OutboundPacket begin_outbound_packet(Session&                  sess,
                                     const OutboundPacketPlan& plan);

// Pad the writer to a byte boundary, return the wire bytes, and update
// session state (commit send_seq bump if applicable, refresh
// last_outbound_ms). Caller is responsible for actually calling send_to().
std::vector<std::uint8_t>
finalize_outbound_packet(Session&         sess,
                         OutboundPacket&& pkt,
                         std::uint64_t    now_ms);

// Stateful convenience wrapper. Construct, append body bits via
// writer(), then call finish() to commit and return the wire bytes.
class OutboundPacketBuilder
{
public:
    OutboundPacketBuilder(Session& sess, const OutboundPacketPlan& plan);

    net20::BitWriter& writer() noexcept { return pkt_.writer; }

    // Useful read-back for body builders that need to know what was put
    // into the header (e.g. logging, retransmit-ring bookkeeping).
    std::uint16_t send_seq() const noexcept { return pkt_.written_send_seq; }
    std::uint8_t  type_word() const noexcept { return pkt_.type_word; }

    // Finalize: pad to byte boundary, commit session state, return bytes.
    std::vector<std::uint8_t> finish(std::uint64_t now_ms);

private:
    Session&        sess_;
    OutboundPacket  pkt_;
    bool            finished_ = false;
};

// Run all clean-room spec §9 test vectors A..E and verify byte-identical
// output, plus a parse_incoming_header round-trip. Returns 0 on success.
int tah_vc_outbound_selftest();

}  // namespace tah_vc

// Re-export the selftest entry point at the dts_viewer namespace so the
// server main.cpp follows the same `dts_viewer::xxx_selftest()` pattern
// as the other selftests.
int tah_vc_outbound_selftest();

}  // namespace dts_viewer

#endif  // OSENGINE_TAH_VC_OUTBOUND_HPP
