#ifndef OSENGINE_TAH_BURST_ORCHESTRATOR_HPP
#define OSENGINE_TAH_BURST_ORCHESTRATOR_HPP

// Spec 26/14c-I-4 — TAH initial-burst orchestrator (clean-room).
//
// Composes the three I-1/I-2/I-3 building blocks (per-session VC outbound
// header, per-class ghost-intro encoders, and the datablock-catalogue
// event encoder) into the post-AcceptConnect initial-state dump that a
// freshly-AC'd TAH session expects.
//
// Wire-format reference: `docs/clean-room-specs/TRIBES-INITIAL-BURST.md`.
//
// Phase ordering implemented here:
//
//   * Phase 0/1 (Handshake / Join-ack) — handled by the caller
//     (server_listener.cpp's AcceptConnect reply path). The orchestrator
//     STARTS at phase 2.
//
//   * Phase 2 (Scope-always burst) — a contiguous run of DataPackets,
//     each built with `tah_vc::OutboundPacketBuilder` so the per-session
//     VC header (parity, send_seq, ack list, type word) is correct. The
//     burst is composed of two interleaved sub-streams:
//       (a) the EVENT sub-stream carries the datablock-catalogue dump —
//           one or more packets emit `stock_tribes_ctf_catalogue()`
//           records via `encode_catalogue_packet`. R-3 §3 / §5 / §7 model
//           the catalogue dump as reliable events on the event channel;
//           R-4 §1.1 explicitly states there is NO separate "datablock
//           phase ordered before ghosts" — datablocks ride the same
//           run of phase-2 packets, in the event sub-stream alongside
//           the ghost-update sub-stream of the ghost-intro packets.
//           SPEC-RECONCILIATION: per R-4's authority we emit catalogue
//           packets FIRST (one packet per soft-cap bucket) so the
//           receiver has the datablock map populated before the ghost
//           intros reference per-class data-file ids. The catalogue
//           packets have ghost-sub-stream-present = 0 (just events).
//       (b) the GHOST-UPDATE sub-stream carries the scope-always ghost
//           introductions for in-world objects:
//             - The local player's session (so TAH has an avatar).
//             - Mission CTF flag-stand markers (when `mission` carries
//               extractable flag positions, via `FlagWorld::load_from_mission`).
//             - Mission static markers (a stub list when no mission is
//               loaded — keeps the burst non-empty for the selftest).
//           These packets have event-sub-stream-present = 0, ghost
//           sub-stream mode-flag = 1 (scope-always). The LAST packet
//           in this sub-burst sets the trailing scope-always-complete
//           bit = 1 per R-4 §3.
//
//   * Phase 3 (Done-ack / steady state) — handled implicitly by the
//     normal per-tick ghost-delta path (mode-flag = 0). The orchestrator
//     does NOT emit a phase-3 packet; it stops after the last scope-
//     always packet.
//
// Output: a vector of complete UDP-ready packet byte vectors, in
// emission order. The caller invokes `socket.send_to(peer, p.data(),
// p.size())` for each in turn.
//
// Selftest: `tah_burst_orchestrator_selftest()` builds a synthetic
// Session, runs `build_initial_burst(sess, nullptr, 0)`, and verifies:
//   * Every packet's VC header parses via `parse_incoming_header`.
//   * The first N packets are catalogue-event packets (event sub-stream
//     present, ghost sub-stream absent).
//   * The remaining packets are scope-always ghost-update packets
//     (mode-flag = 1).
//   * The last ghost-update packet's trailing scope-always-complete bit
//     reads as 1.
//   * Total bytes are roughly within ±30% of the ground-truth pcap's
//     burst size (8809 B for the 31-packet snapshot — i.e. [6166, 11452]).

#pragma once

#include <osengine/session_table.hpp>

#include <cstdint>
#include <vector>

namespace dts_viewer
{

struct LoadedMission;

class TahBurstOrchestrator
{
public:
    // Catalogue event-class tag for stock Tribes 1.41 / TAH. R-3 §7
    // names this as 88 for TAH captures; it is per-build, so callers
    // wanting to override may set this directly before calling.
    std::uint16_t catalogue_event_class_tag = 88;

    // Starting per-channel event seq for the catalogue dump. The
    // event sub-stream uses its own ordered-channel seq, distinct from
    // the VC send_seq. Defaults to 9 to match the TAH capture's first
    // catalogue-bearing packet (see R-3 §3.5 worked example).
    std::uint8_t catalogue_first_event_seq = 9;

    // Build the complete initial-state dump for `sess`. Returns the
    // wire-ready packet bytes in emission order. The caller is
    // responsible for socket I/O.
    //
    // `mission` is optional. When present, mission flag stands are
    // emitted as FlagStand-style scope-always markers; when null, a
    // minimal stub set of ghosts is emitted so the burst is non-empty
    // (used by the selftest and by handshake-only test deployments).
    std::vector<std::vector<std::uint8_t>>
    build_initial_burst(Session& sess,
                        const LoadedMission* mission,
                        std::uint64_t now_ms);
};

// Run the orchestrator selftest. Returns 0 on success.
int tah_burst_orchestrator_selftest();

}  // namespace dts_viewer

#endif  // OSENGINE_TAH_BURST_ORCHESTRATOR_HPP
