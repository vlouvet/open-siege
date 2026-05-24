#ifndef OSENGINE_GHOST_EMITTER_HPP
#define OSENGINE_GHOST_EMITTER_HPP

// Spec 28/04 — per-client ghost emission, v1.
//
// Each session owns one GhostEmitter. On every server tick the listener
// builds a ServerWorldSnapshot and calls emit() on every active
// emitter. The emitter:
//   1. Walks the snapshot, computes which ghosts have moved since the
//      client's last-acknowledged version.
//   2. Encodes a compact "OS Ghost Burst" datagram covering the dirty
//      set (capped by MTU; v1 fits ≤30 players in one packet).
//   3. Hands the buffer to a sink callback that puts it on the wire.
//
// v1 scope note (deferred to spec 28/04b):
//   * The wire format is an OPEN-SIEGE NATIVE layout — magic 'O','S','G','B'
//     after a VC header — NOT the T1 1.41 byte-compatible ghost stream.
//     This lets us validate the per-session orchestration + delta
//     propagation independently of the encoder-completeness work
//     (~week of careful per-class bit inversion against §15 of the
//     clean-room netproto spec).
//   * Sessions still receive the captured-template 223-byte burst on
//     their first DataPacket (spec 26/11) unless --legacy-canned-burst
//     is explicitly disabled. The OSGB stream is layered ON TOP.
//   * No relevance filtering, no priority queue: every client sees
//     every other client every emit cycle.

#include "content/net/udp_socket.hpp"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace dts_viewer
{

struct Session;
struct ServerWorldSnapshot;

// What each client knows about a given ghost. Held per-session so the
// next emit only re-sends genuinely-changed fields.
struct GhostViewEntry
{
    float    last_pos_x = 0.0f;
    float    last_pos_y = 0.0f;
    float    last_pos_z = 0.0f;
    float    last_yaw   = 0.0f;
    std::uint16_t last_sent_seq = 0;     // VC send-seq of last packet that touched this ghost
    bool     ever_sent = false;          // false → emit forces-include this ghost
};

struct GhostEmitterStats
{
    std::uint64_t packets_emitted   = 0;
    std::uint64_t bytes_emitted     = 0;
    std::uint64_t records_emitted   = 0;
    std::uint64_t kills_emitted     = 0;   // count of "ghost-removed" records
};

class GhostEmitter
{
public:
    // The sink takes (peer, bytes, len) → bool sent_ok. The listener
    // binds it to ServerListener::send_to() under the listener mutex.
    using Sink = std::function<bool(const studio::content::net::Endpoint&,
                                    const std::uint8_t*, std::size_t)>;

    GhostEmitter(Session* session, Sink sink);

    // Walk the snapshot, build + send one OSGB packet for this client.
    // Caller is responsible for tick-rate throttling (default cadence
    // is "every other tick" at 32 Hz → ~15 Hz, well under T1's 30 Hz
    // baseline but enough to demonstrate the pipeline).
    void emit(const ServerWorldSnapshot& world);

    // Mark every ghost field whose last_sent_seq <= recv_seq as
    // acknowledged. v1 doesn't dispatch per-field; we use this hook
    // for stats only. Real per-field ack tracking is 28/04b.
    void on_client_ack(std::uint16_t recv_seq);

    const GhostEmitterStats& stats() const noexcept { return stats_; }

private:
    Session* session_;
    Sink     sink_;
    std::unordered_map<std::uint16_t, GhostViewEntry> view_;
    GhostEmitterStats stats_;
};

// Wire-format header for the OS Ghost Burst packet, sent immediately
// after the VC header (4 bytes) when type-word == kOpenSiegeGhostBurst.
//
//   offset 0..3 : VC header (4B, byte-aligned)
//   offset 4..7 : 'O','S','G','B'
//   offset 8    : version (currently 0x01)
//   offset 9    : record count N
//   offset 10..17 : server_time_ms (u64 LE)
//   offset 18 + N*kOSGBRecordSize bytes : N records
//
// Each record (24 bytes):
//   u16 LE ghost_id
//   u8     flags  (bit 0 = kill, bit 1 = on_ground)
//   u8     team_id
//   f32 LE pos_x
//   f32 LE pos_y
//   f32 LE pos_z
//   f32 LE yaw
//   u8     damage (0..255 normalised)
//   u8     anim_index
//   u8     pad[2]
//
// VC type-word value: 0x11 (≠ kDataPacket=0, kPureAck=0x10, kPing=7).
// Marks the packet as OSGB so decoders can dispatch.
constexpr std::uint8_t kOpenSiegeGhostBurstType = 0x11;
constexpr std::size_t  kOSGBHeaderSize = 18;
constexpr std::size_t  kOSGBRecordSize = 24;

struct OSGBRecord
{
    std::uint16_t ghost_id = 0;
    std::uint8_t  flags = 0;             // bit 0 = kill, bit 1 = on_ground
    std::uint8_t  team_id = 0;
    float         pos_x = 0.0f;
    float         pos_y = 0.0f;
    float         pos_z = 0.0f;
    float         yaw = 0.0f;
    std::uint8_t  damage = 0;
    std::uint8_t  anim_index = 0;
};

struct OSGBPacket
{
    std::uint16_t      vc_send_seq = 0;
    std::uint8_t       vc_parity = 0;
    std::uint64_t      server_time_ms = 0;
    std::vector<OSGBRecord> records;
};

// Encoder — used by GhostEmitter::emit. Exposed for the selftest.
std::vector<std::uint8_t> encode_osgb_packet(const OSGBPacket& pkt);

// Decoder — round-trip companion. Returns false if magic / size / pad
// don't validate.
bool decode_osgb_packet(const std::uint8_t* data, std::size_t size,
                        OSGBPacket& out);

// Run the synchronous in-process selftest:
//   * Spawn 3 sessions at distinct positions.
//   * Tick 5 times; each tick call emit() on every session.
//   * Capture sent packets into an in-memory sink.
//   * Assert each session emitted ≥4 packets (one of which carries a
//     full intro for all 3 ghosts).
//   * Re-decode the bytes and assert position fidelity.
int ghost_emitter_selftest();

} // namespace dts_viewer

#endif // OSENGINE_GHOST_EMITTER_HPP
