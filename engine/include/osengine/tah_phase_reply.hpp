// 14c-PhaseA — TAH Phase 1 reply + full catalogue burst (clean-room).
//
// Authority: docs/clean-room-specs/TRIBES-FULL-PROTOCOL-SPEC.md
//   * §1.5 BitStream conventions
//   * §1.6 Per-packet structure
//   * §2.1 Event SubStream framing
//   * §2.6 SimConsoleEvent
//   * §2.9 PlayerAddEvent
//   * §2.12 TeamAddEvent
//   * §4 DataBlockEvent + per-type bodies
//   * §6.4 Ping reply
//   * §7.4..§7.7 bit-by-bit walk
//   * §8 P1 / P2 / P5 implementer mandate
//
// What this module provides:
//
//   1. `is_setclinfo_clientready(buf)` — detect TAH's ClientReady packet
//      by scanning the inbound C->S DataPacket for a SimConsoleEvent
//      remoteEval whose command is "SetCLInfo".
//   2. `build_phase1_reply(sess, now_ms)` — emit the Phase 1 DataPacket
//      with R1=1 max_delay=33 max_size=450, then 3 TeamAddEvents, 1
//      PlayerAddEvent, and 2 SimConsoleEvents (SVInfo, MODInfo). Wire
//      shape matches §7.5.
//   3. `build_catalogue_burst(sess, now_ms)` — emit the full Blastside
//      catalogue (~430 records) as DataBlockEvents (wire tag 88) packed
//      into ~400 B packets. The first packet carries R0=1 cur_delay=66
//      cur_size=400; the last packet's last event is the IrcChannelData
//      sentinel (db_type=30, group_size=0, block_index=0xff) which
//      triggers TAH's dataFinished.
//   4. `build_ping_reply(sess, their_seq, now_ms)` — 4-byte VC ack reply
//      for a ptype=7 Ping.

#pragma once

#include <osengine/session_table.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace dts_viewer {

// True iff `buf` is a C->S DataPacket whose ESS contains a
// SimConsoleEvent with command "SetCLInfo". Stateless — does not
// modify the session.
bool is_setclinfo_clientready(const std::vector<std::uint8_t>& buf);

// Build the Phase 1 reply per §7.5 / §8 P1. Mutates `sess.next_send_seq`
// (bumps once) and `sess.last_outbound_ms`.
std::vector<std::uint8_t>
build_phase1_reply(Session& sess, std::uint64_t now_ms);

// Build the full catalogue burst per §4.32 / §8 P2. Returns one byte
// vector per packet. Mutates `sess.next_send_seq` (bumps once per
// packet) and `sess.last_outbound_ms`.
//
// The first packet carries the R0=1 cur_delay=66 cur_size=400 rate-
// control announcement; subsequent packets have R0=R1=0. Each packet's
// ESS is closed with the trailing 0-bit; PSC=0 and GSS=0 follow.
//
// The LAST packet's LAST event is the IrcChannelData sentinel
// (db_type=30, group_size=0, block_index=0xff), which is what triggers
// TAH's dataFinished signal and the load-screen-completion path.
std::vector<std::vector<std::uint8_t>>
build_catalogue_burst(Session& sess, std::uint64_t now_ms);

// Build a 4-byte VC pure-ack reply for a Ping (ptype=7) per §6.4. The
// reply reuses the current send_seq (Ping doesn't bump per §6.4) and
// echoes the inbound `their_seq` in hrcv. Mutates `sess.last_outbound_ms`.
std::vector<std::uint8_t>
build_ping_reply(Session& sess, std::uint16_t their_seq, std::uint64_t now_ms);

// 14c-PhaseA selftest — wire-shape sanity (envelope only).
int tah_phase_reply_selftest();

}  // namespace dts_viewer
