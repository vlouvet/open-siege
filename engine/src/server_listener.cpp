#include <osengine/server_listener.hpp>

#include <osengine/ghost_emitter.hpp>
#include <osengine/tah_burst_orchestrator.hpp>
#include <osengine/tah_ghost_burst.hpp>
#include <osengine/tah_phase_reply.hpp>
#include <osengine/movecommand.hpp>
#include <osengine/net_client.hpp>     // spec 29/02b — server_info codec
#include <osengine/server_world_snapshot.hpp>
#include <osengine/session_table.hpp>
#include <osengine/team_assigner.hpp>
#include "content/net/udp_socket.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace dts_viewer
{

namespace
{

// AcceptConnect template captured from a real Tribes 1.41 dedicated
// server's reply on loopback handshake (2026-05-21 capture, see
// captures/real-tribes/handshake-full-2026-05-21.json). Bytes 4..6 are
// the nonce slot — overwritten with the value extracted from the
// inbound RequestConnect's bytes 7..9.
constexpr std::uint8_t kAcceptConnectTemplate[16] = {
    0x07, 0x00, 0x09, 0x60,
    0x00, 0x00, 0x00,                // <- nonce slot (bytes 4..6)
    0x12, 0x01, 0x08, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00,
};

// Spec 26/10b — 18-byte Groove AcceptConnect template captured from
// captures/real-tribes/groove-session-20260522-124329.json packet 1.
// Bytes 4..6 = per-session nonce echo (mirrors vanilla layout). The
// trailing 11 bytes are constants from the capture; meaning not
// independently verified, but bit-exact replay works with the
// net-test-client --groove probe.
constexpr std::uint8_t kGrooveAcceptConnectTemplate[18] = {
    0x07, 0x00, 0x09, 0x60,
    0x00, 0x00, 0x00,                // <- nonce slot (bytes 4..6)
    0x00, 0x02,
    0x08, 0x00, 0x00, 0x00, 0x08, 0x00, 0x80, 0x01, 0x01,
};

// First-ghost-burst template captured from the same handshake (third
// packet, server->client, 223 bytes, type byte 0x0b = data_with_payload
// in VC framing). Sent back to the client when we observe its first
// DataPacket so phase-3 counts at least one ghost packet — spec 26/11.
constexpr std::uint8_t kFirstGhostBurstTemplate[223] = {
    0x0b, 0x08, 0x09, 0x00, 0x5c, 0xc0, 0x13, 0x00, 0x00, 0x00, 0xe0, 0xff,
    0xff, 0xff, 0xff, 0x81, 0x23, 0x4a, 0xf4, 0x21, 0xc1, 0x2e, 0xdb, 0xff,
    0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x21, 0x62, 0xa8,
    0x86, 0x8b, 0x67, 0x3f, 0xbe, 0xc1, 0xee, 0xb3, 0x1f, 0xff, 0x27, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0xc0, 0x06, 0x5f, 0x4e, 0xa4, 0x14,
    0x6e, 0x5a, 0xa8, 0x85, 0x06, 0xa1, 0x15, 0x6a, 0xe1, 0xcb, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x12, 0xec, 0xb2, 0x3d, 0x00, 0xff, 0xff, 0xff, 0xff,
    0x8e, 0x30, 0x0d, 0xa6, 0xeb, 0x1e, 0x45, 0x9b, 0x1e, 0x10, 0xcb, 0x35,
    0x36, 0xac, 0xba, 0xa2, 0x22, 0xe2, 0xd3, 0x9b, 0xbe, 0x49, 0x6f, 0x09,
    0x76, 0xd9, 0x7e, 0x8e, 0xef, 0x6f, 0xe9, 0x06, 0xdc, 0xaa, 0x2b, 0x2a,
    0x22, 0x3e, 0xbd, 0xf6, 0x26, 0xbd, 0x5d, 0x7b, 0x70, 0xd5, 0xba, 0x3a,
    0x03, 0x62, 0x5a, 0x7b, 0x72, 0xa7, 0x09, 0x69, 0x41, 0xeb, 0x8a, 0xa3,
    0xe4, 0xb1, 0xf6, 0xe4, 0x4e, 0x13, 0xd2, 0x82, 0x00, 0x1c, 0x21, 0x1e,
    0x78, 0xd7, 0xf4, 0x8d, 0xa2, 0x4d, 0x01, 0x1c, 0x21, 0x1e, 0xd4, 0xe5,
    0xf1, 0x72, 0xef, 0x92, 0x07, 0x70, 0x2c, 0xa0, 0x00, 0x00, 0x02, 0x00,
    0x01, 0x00, 0x40, 0x82, 0x02, 0x00, 0x30, 0x0a, 0x1d, 0x0b, 0x28, 0x04,
    0x00, 0x00, 0x00, 0xc7, 0x02, 0x0a, 0x02, 0x02, 0x00, 0xc0, 0xb1, 0x80,
    0xc2, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x05, 0x0a, 0x00, 0x00, 0x22,
    0x24, 0xfc, 0xff, 0xff, 0xff, 0x6b, 0x3a,
};

// 27-byte vanilla RequestConnect detection: starts with 0x07.
bool looks_like_vanilla_request_connect(const std::vector<std::uint8_t>& pkt)
{
    return pkt.size() == 27 && pkt[0] == 0x07;
}

// 45-byte Groove RequestConnect. Two known leading bytes:
//   * 0x07  — Tribes 1 proxy capture (groove-session-20260522-124329.json
//             packet 0); per-session nonce at offsets 10..12.
//   * 0x05  — TribesAfterHope.exe variant observed 2026-05-24; layout
//             not yet byte-mapped (pcap missing the 45B run). Treat the
//             same wrt session allocation; reuse offsets 10..12 for the
//             nonce (best-effort until cross-referenced).
bool looks_like_groove_request_connect(const std::vector<std::uint8_t>& pkt)
{
    return pkt.size() == 45 && (pkt[0] == 0x05 || pkt[0] == 0x07);
}

// TribesAfterHope RequestConnect family — UNIVERSAL classifier (R-8).
//
// TAH cycles through multiple outer framings when probing a server.
// The 2026-06-16 R-8 analysis (8 captured shapes across 5 sessions)
// found a universal byte-layout invariant: every shape decomposes as
//
//   [N-byte prefix] [1B parity] [3B nonce] [1B separator] [38B body]
//
// where the prefix length N varies (3 for Shape E up to 11 for Shape G)
// but the trailing 5+38 = 43 bytes are positioned identically relative
// to packet end. Solving N + 5 + 38 = total_size:
//
//   parity_off = total_size - 43
//   nonce_off  = total_size - 42  (3 bytes)
//   separator  = total_size - 39  (1 byte, high 5 bits = 0x18; observed
//                                   low 3 bits 000/001/100/101 across
//                                   sessions — likely a per-field parity)
//   body_off   = total_size - 38  (38 bytes, encrypted Groove blob)
//
// Verified shape catalogue (sessions 2026-05-21 .. 2026-06-16):
//   Shape A  — 53B, prefix 05 00 21 41 61 81 a1 c1 e1 01
//   Shape B  — 51B, prefix 05 00 25 61 81 a2 c1 e5
//   Shape C  — 53B, prefix 07 00 21 41 61 81 a1 c1 e1 01
//   Shape D  — 52B, prefix [05|07] 00 11 23 47 83 a7 e3 01  (sep = 0x1c)
//   Shape E  — 46B, prefix 07 00 69
//   Shape E' — 46B, prefix 05 00 09
//   Shape F  — 50B, prefix [05|07] 00 17 4f 87 bf f3
//   Shape G  — 54B, prefix 07 00 11 27 59 71 82 b1 c2 e2 01
//
// The discriminator is the parity byte at (total_size - 43): every
// retransmit-pair captured toggles 0x18 ↔ 0x58 at that position
// (binary 0001 1000 ↔ 0101 1000 — the high bits encode a 2-bit parity
// counter; the AC encoder consumes only the byte value).
//
// Matcher strategy: structural classifier — no prefix list required.
//   1. Size in [46, 60]   (excludes Groove's exact 45B at the bottom,
//                          stops shy of normal data-packet sizes at top)
//   2. Bytes 0..1 = (0x05|0x07), 0x00
//   3. Parity byte (size - 43) in {0x18, 0x58}
//   4. Separator byte (size - 39) has high 5 bits == 0x18
//      (observed values 0x18, 0x19, 0x1c, 0x1d across 5 sessions)
//
// The classifier intentionally does NOT constrain on the 38B body or
// the prefix bytes — TAH may introduce new framings, but the universal
// layout has held across every observed session. See R-8 spec at
// docs/done/26-three-binaries/14c-R-8-... for the full bit evidence.
bool looks_like_tah_request_connect(const std::vector<std::uint8_t>& pkt,
                                    std::size_t* out_parity_off,
                                    std::size_t* out_nonce_off)
{
    const std::size_t n = pkt.size();
    if (n < 46 || n > 60) return false;
    if (pkt[0] != 0x05 && pkt[0] != 0x07) return false;
    if (pkt[1] != 0x00) return false;
    const std::uint8_t parity = pkt[n - 43];
    if (parity != 0x18 && parity != 0x58) return false;
    const std::uint8_t sep = pkt[n - 39];
    if ((sep & 0xf8) != 0x18) return false;
    if (out_parity_off) *out_parity_off = n - 43;
    if (out_nonce_off)  *out_nonce_off  = n - 42;
    return true;
}

// 4-byte DataPacket / pure-ack shapes. Bytes 0..2 are mostly fixed
// (`?? 08 09 ??`); byte 0 high nibble = parity, byte 3 encodes the
// 5-bit packet type at bits 27..31.
//   * vanilla Tribes 1.41: 07 08 09 80 (type=16, Ack)
//   * vanilla parity=0:    05 08 09 80
//   * TAH client:          07 08 09 38 (type=7)
//   * TAH parity=0:        05 08 09 38
// Recognise all of them as session-keep-alive pure-acks.
bool looks_like_first_data_packet(const std::vector<std::uint8_t>& pkt)
{
    return pkt.size() == 4
        && (pkt[0] == 0x07 || pkt[0] == 0x05)
        && pkt[1] == 0x08 && pkt[2] == 0x09
        && (pkt[3] == 0x80 || pkt[3] == 0x38);
}

// RejectConnect template for "Server is full." (spec 28/01). Per the
// BREAKTHROUGH server-reply structure: bytes 0-1 echo request 0-1
// (07 00), bytes 2-3 are 09 28 (REJECT flags), bytes 4-6 are the
// nonce echo, byte 7 is 12 (request byte-10 echo), bytes 8+ are the
// null-terminated ASCII reason.
//   "Server is full.\0" = 0x53 0x65 0x72 0x76 0x65 0x72 0x20 0x69
//                         0x73 0x20 0x66 0x75 0x6c 0x6c 0x2e 0x00
// Total: 8 (header) + 16 (reason) = 24 bytes.
constexpr std::uint8_t kRejectServerFullTemplate[24] = {
    0x07, 0x00, 0x09, 0x28,
    0x00, 0x00, 0x00,                  // <- nonce slot (bytes 4..6)
    0x12,
    0x53, 0x65, 0x72, 0x76, 0x65, 0x72, 0x20, 0x69,
    0x73, 0x20, 0x66, 0x75, 0x6c, 0x6c, 0x2e, 0x00,
};

void build_reject_server_full(const std::uint8_t nonce[3], std::uint8_t out[24])
{
    std::memcpy(out, kRejectServerFullTemplate, 24);
    out[4] = nonce[0];
    out[5] = nonce[1];
    out[6] = nonce[2];
}

std::uint64_t steady_now_ms()
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void hex_prefix(const std::vector<std::uint8_t>& pkt, char* buf, std::size_t bufsz)
{
    const std::size_t n = std::min(pkt.size(), std::size_t{64});
    std::size_t off = 0;
    for (std::size_t i = 0; i < n && off + 3 < bufsz; ++i) {
        int w = std::snprintf(buf + off, bufsz - off, "%02x ", pkt[i]);
        if (w <= 0) break;
        off += static_cast<std::size_t>(w);
    }
    if (off && off < bufsz) buf[off - 1] = '\0';
}

} // anonymous namespace

void build_accept_connect_reply(const std::uint8_t nonce[3], std::uint8_t out[16])
{
    std::memcpy(out, kAcceptConnectTemplate, 16);
    out[4] = nonce[0];
    out[5] = nonce[1];
    out[6] = nonce[2];
}

void build_groove_accept_connect_reply(const std::uint8_t nonce[3],
                                       std::uint8_t out[18])
{
    std::memcpy(out, kGrooveAcceptConnectTemplate, 18);
    out[4] = nonce[0];
    out[5] = nonce[1];
    out[6] = nonce[2];
}

// 14c-I-pcap-diff (TRIBES-PROTOCOL-PCAP-DIFF.md §2.5) — public TAH
// server's 16-byte AcceptConnect form. The public server emits:
//
//   byte 0  = 0x05 | (parity_bit << 1)   where parity_bit = nonce[0] & 1
//   byte 1  = 0x00 (fresh session — see §2.3 note re mid-session reconnect)
//   byte 2  = 0x09
//   byte 3  = 0x60
//   bytes 4..6 = nonce echo
//   byte 7  = separator_byte echo (RC byte at total_size - 39)
//   byte 8  = (separator_byte & 0x07) << 1
//   bytes 9..15 = { 0x08, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00 }
//
// Replaces the prior 18-byte form (with trailing 0x80 0x01 0x01) which
// TAH silently rejected at the application layer.
void build_tah_accept_connect_reply(const std::uint8_t nonce[3],
                                    std::uint8_t      separator_byte,
                                    std::uint8_t      out[16])
{
    const std::uint8_t parity_bit = static_cast<std::uint8_t>(nonce[0] & 0x01u);
    out[0]  = static_cast<std::uint8_t>(0x05u | (parity_bit << 1));
    out[1]  = 0x00;
    out[2]  = 0x09;
    out[3]  = 0x60;
    out[4]  = nonce[0];
    out[5]  = nonce[1];
    out[6]  = nonce[2];
    out[7]  = separator_byte;
    out[8]  = static_cast<std::uint8_t>((separator_byte & 0x07u) << 1);
    out[9]  = 0x08;
    out[10] = 0x00;
    out[11] = 0x00;
    out[12] = 0x00;
    out[13] = 0x08;
    out[14] = 0x00;
    out[15] = 0x00;
}

namespace
{
struct EmitterKey
{
    std::string host;
    std::uint16_t port = 0;
    bool operator==(const EmitterKey& o) const noexcept {
        return port == o.port && host == o.host;
    }
};
struct EmitterKeyHash
{
    std::size_t operator()(const EmitterKey& k) const noexcept {
        return std::hash<std::string>{}(k.host) ^ (std::size_t(k.port) * 0x9E3779B97F4A7C15ull);
    }
};
} // namespace

struct ServerListener::Impl
{
    studio::content::net::UdpSocket socket;
    mutable std::mutex              mu;
    ServerListenerStats             stats;
    std::string                     last_error;
    std::unique_ptr<SessionTable>   sessions;
    std::uint64_t                   tick_counter = 0;
    std::unordered_map<EmitterKey, std::unique_ptr<GhostEmitter>,
                       EmitterKeyHash>  emitters;
    std::vector<SpawnPoint>         spawns;       // spec 28/05
    std::string                     mission_name; // spec 29/02b
    const LoadedMission*            loaded_mission = nullptr; // 14c-I-7 followup
    std::uint8_t                    last_keepalive_byte3 = 0xff; // 26/10b dedup
};

void ServerListener::set_spawn_points(std::vector<SpawnPoint> spawns)
{
    impl_->spawns = std::move(spawns);
}

const std::vector<SpawnPoint>& ServerListener::spawn_points() const
{
    return impl_->spawns;
}

void ServerListener::set_mission_name(std::string name)
{
    impl_->mission_name = std::move(name);
}

void ServerListener::set_loaded_mission(const LoadedMission* mission)
{
    impl_->loaded_mission = mission;
}

ServerListener::ServerListener(ServerListenerConfig cfg)
    : cfg_(cfg), impl_(std::make_unique<Impl>())
{
    impl_->sessions = std::make_unique<SessionTable>(cfg_.max_players);
}

ServerListener::~ServerListener()
{
    stop();
}

bool ServerListener::start()
{
    if (running_.load()) return true;
    if (!impl_->socket.bind(cfg_.port)) {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->last_error = impl_->socket.last_error();
        std::fprintf(stderr, "[listener] bind on %u failed: %s\n",
                     cfg_.port, impl_->last_error.c_str());
        return false;
    }
    std::fprintf(stderr, "[listener] bound on 0.0.0.0:%u\n", cfg_.port);
    quit_.store(false);
    running_.store(true);
    thread_ = std::thread([this] { run(); });
    return true;
}

void ServerListener::stop()
{
    if (!running_.load() && !thread_.joinable()) return;
    quit_.store(true);
    if (thread_.joinable()) thread_.join();
    impl_->socket.close();
    running_.store(false);
}

ServerListenerStats ServerListener::stats() const
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->stats;
}

std::string ServerListener::last_error() const
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->last_error;
}

SessionTable& ServerListener::sessions()
{
    return *impl_->sessions;
}

void ServerListener::run()
{
    using namespace std::chrono;
    using studio::content::net::Endpoint;

    const auto period = milliseconds(1000 / std::max(1, cfg_.tick_hz));
    std::vector<std::uint8_t> buf;
    Endpoint peer;
    std::uint8_t accept_reply[16];
    std::uint8_t reject_reply[24];

    while (!quit_.load()) {
        const auto t0 = steady_clock::now();
        const std::uint64_t now_ms = steady_now_ms();

        // Drain inbound queue. try_recv returns false when queue empty.
        while (impl_->socket.try_recv(buf, peer)) {
            // 26/14a — if this packet is from an established session
            // and looks like a VC datagram, decode its send_seq and
            // mark the slot for ack-back. Handshake packets handled
            // below have their own ack semantics (the AC reply takes
            // care of the RequestConnect's seq), so we skip them here.
            //
            // 14c-PhaseA P5: Ping (ptype=7) packets used to be dropped
            // silently. We now intercept them here, send a 4-byte
            // pure-ack reply per spec §6.4, and `continue` so the rest
            // of the classifier doesn't see the packet.
            if (buf.size() >= 4 && (buf[0] & 1) && impl_->sessions->find(peer)) {
                net20::ParsedIncomingHeader ph;
                if (net20::parse_incoming_header(buf.data(), buf.size(), ph)) {
                    Session* sess = impl_->sessions->find(peer);
                    if (ph.base_type == net20::pkt_type::kPing) {
                        // 14c-PhaseA P5 — Ping reply.
                        impl_->sessions->touch(peer, now_ms);
                        auto reply = build_ping_reply(*sess, ph.send_seq, now_ms);
                        if (impl_->socket.send_to(peer, reply.data(), reply.size())) {
                            sess->last_outbound_ms = now_ms;
                        }
                        std::lock_guard<std::mutex> lk(impl_->mu);
                        ++impl_->stats.data_packets_received;
                        continue;
                    }
                    sess->ack.on_receive(ph.send_seq);
                }
            }

            if (looks_like_vanilla_request_connect(buf)) {
                const std::uint8_t nonce[3] = { buf[7], buf[8], buf[9] };
                const bool was_new = impl_->sessions->find(peer) == nullptr;
                Session* sess = impl_->sessions->allocate(peer, nonce, now_ms);
                if (sess) {
                    // Spec 28/05 — only assign on first allocation; a
                    // RequestConnect retransmit must not re-randomise
                    // the player's spawn.
                    if (was_new) {
                        sess->team = pick_team(*impl_->sessions, cfg_.team_balance);
                        place_at_spawn(*sess, impl_->spawns);
                        const char* tn = (sess->team == Team::Red)  ? "red"
                                       : (sess->team == Team::Blue) ? "blue"
                                       : "spec";
                        std::fprintf(stderr,
                            "[spawn] slot %u -> team %s at (%.1f, %.1f, %.1f) yaw=%.2f\n",
                            sess->player_slot, tn,
                            sess->spawn_pos.x, sess->spawn_pos.y, sess->spawn_pos.z,
                            sess->spawn_yaw);
                    }
                    // New or retransmit — reply with AcceptConnect using
                    // the session's nonce (so retransmits reuse it).
                    build_accept_connect_reply(sess->nonce, accept_reply);
                    sess->connect_parity = (accept_reply[0] & 0x02) != 0;
                    const bool ok = impl_->socket.send_to(
                        peer, accept_reply, sizeof(accept_reply));
                    if (ok) sess->last_outbound_ms = now_ms;
                    std::lock_guard<std::mutex> lk(impl_->mu);
                    ++impl_->stats.request_connects_received;
                    if (ok) {
                        ++impl_->stats.accept_connects_sent;
                        std::fprintf(stderr,
                            "[listener] RequestConnect from %s:%u -> slot %u, replied AcceptConnect (nonce %02x%02x%02x)\n",
                            peer.host.c_str(), peer.port, sess->player_slot,
                            sess->nonce[0], sess->nonce[1], sess->nonce[2]);
                        // 14c-I-pcap-diff §3 — the public TAH server emits
                        // no server_info packet after AC. Removed here for
                        // wire-parity; mission-name publish belongs in the
                        // scope-always burst.
                        (void)was_new;
                    } else {
                        impl_->last_error = impl_->socket.last_error();
                        std::fprintf(stderr,
                            "[listener] send_to %s:%u failed: %s\n",
                            peer.host.c_str(), peer.port, impl_->last_error.c_str());
                    }
                } else {
                    // Table full — reply with RejectConnect("Server is full.").
                    build_reject_server_full(nonce, reject_reply);
                    const bool ok = impl_->socket.send_to(
                        peer, reject_reply, sizeof(reject_reply));
                    std::lock_guard<std::mutex> lk(impl_->mu);
                    ++impl_->stats.request_connects_received;
                    if (ok) {
                        ++impl_->stats.reject_connects_sent;
                        std::fprintf(stderr,
                            "[listener] RequestConnect from %s:%u, replied RejectConnect (server full)\n",
                            peer.host.c_str(), peer.port);
                    } else {
                        impl_->last_error = impl_->socket.last_error();
                    }
                }
            }
            else if (looks_like_groove_request_connect(buf)) {
                // Spec 26/10b — Groove handshake. Per-session nonce at
                // offsets 10..12 (verified from the 0x07-leading proxy
                // capture; reused for the 0x05 TAH variant pending a
                // clean byte map). Same session-allocate flow as
                // vanilla, just with the 18-byte reply template.
                const std::uint8_t nonce[3] = { buf[10], buf[11], buf[12] };
                const bool was_new = impl_->sessions->find(peer) == nullptr;
                Session* sess = impl_->sessions->allocate(peer, nonce, now_ms);
                std::uint8_t groove_reply[18];
                if (sess) {
                    sess->is_tah_session = true;
                    if (was_new) {
                        sess->team = pick_team(*impl_->sessions, cfg_.team_balance);
                        place_at_spawn(*sess, impl_->spawns);
                        const char* tn = (sess->team == Team::Red)  ? "red"
                                       : (sess->team == Team::Blue) ? "blue"
                                       : "spec";
                        std::fprintf(stderr,
                            "[spawn] slot %u -> team %s at (%.1f, %.1f, %.1f) yaw=%.2f\n",
                            sess->player_slot, tn,
                            sess->spawn_pos.x, sess->spawn_pos.y, sess->spawn_pos.z,
                            sess->spawn_yaw);
                    }
                    build_groove_accept_connect_reply(sess->nonce, groove_reply);
                    sess->connect_parity = (groove_reply[0] & 0x02) != 0;
                    const bool ok = impl_->socket.send_to(
                        peer, groove_reply, sizeof(groove_reply));
                    if (ok) sess->last_outbound_ms = now_ms;
                    std::lock_guard<std::mutex> lk(impl_->mu);
                    ++impl_->stats.request_connects_received;
                    if (ok) {
                        ++impl_->stats.accept_connects_sent;
                        std::fprintf(stderr,
                            "[listener] (Groove) RequestConnect from %s:%u -> slot %u, replied AcceptConnect (nonce %02x%02x%02x)\n",
                            peer.host.c_str(), peer.port, sess->player_slot,
                            sess->nonce[0], sess->nonce[1], sess->nonce[2]);
                        // 14c-I-pcap-diff §3 — public TAH server emits no
                        // server_info packet after AC; removed here to
                        // match wire behaviour.
                        (void)was_new;
                    } else {
                        impl_->last_error = impl_->socket.last_error();
                        std::fprintf(stderr,
                            "[listener] (Groove) send_to %s:%u failed: %s\n",
                            peer.host.c_str(), peer.port, impl_->last_error.c_str());
                    }
                } else {
                    // Table full — reply with RejectConnect just like
                    // vanilla. The 24-byte RejectConnect template ends
                    // up indistinguishable from vanilla on the wire;
                    // TAH should at least surface "Server is full" to
                    // the user rather than silently retry.
                    build_reject_server_full(nonce, reject_reply);
                    const bool ok = impl_->socket.send_to(
                        peer, reject_reply, sizeof(reject_reply));
                    std::lock_guard<std::mutex> lk(impl_->mu);
                    ++impl_->stats.request_connects_received;
                    if (ok) {
                        ++impl_->stats.reject_connects_sent;
                        std::fprintf(stderr,
                            "[listener] (Groove) RequestConnect from %s:%u, replied RejectConnect (server full)\n",
                            peer.host.c_str(), peer.port);
                    } else {
                        impl_->last_error = impl_->socket.last_error();
                    }
                }
            }
            else if (std::size_t parity_off = 0, nonce_off = 0;
                     looks_like_tah_request_connect(buf, &parity_off, &nonce_off)) {
                // 14c-I-pcap-diff — TribesAfterHope connect family.
                // Per-shape nonce + parity offsets supplied by the
                // predicate; separator byte at (nonce_off + 3) per
                // TRIBES-PROTOCOL-PCAP-DIFF.md §2.2. Reply is the 16B
                // AC form the public TAH server emits (§2.5).
                const std::uint8_t nonce[3] = {
                    buf[nonce_off], buf[nonce_off + 1], buf[nonce_off + 2],
                };
                const std::uint8_t req_parity   = buf[parity_off];
                const std::uint8_t separator    = buf[nonce_off + 3];
                const bool was_new = impl_->sessions->find(peer) == nullptr;
                Session* sess = impl_->sessions->allocate(peer, nonce, now_ms);
                std::uint8_t tah_reply[16];
                if (sess) {
                    sess->is_tah_session = true;
                    if (was_new) {
                        sess->team = pick_team(*impl_->sessions, cfg_.team_balance);
                        place_at_spawn(*sess, impl_->spawns);
                        const char* tn = (sess->team == Team::Red)  ? "red"
                                       : (sess->team == Team::Blue) ? "blue"
                                       : "spec";
                        std::fprintf(stderr,
                            "[spawn] slot %u -> team %s at (%.1f, %.1f, %.1f) yaw=%.2f\n",
                            sess->player_slot, tn,
                            sess->spawn_pos.x, sess->spawn_pos.y, sess->spawn_pos.z,
                            sess->spawn_yaw);
                    }
                    build_tah_accept_connect_reply(sess->nonce, separator, tah_reply);
                    sess->connect_parity = (tah_reply[0] & 0x02) != 0;
                    const bool ok = impl_->socket.send_to(
                        peer, tah_reply, sizeof(tah_reply));
                    if (ok) sess->last_outbound_ms = now_ms;
                    std::lock_guard<std::mutex> lk(impl_->mu);
                    ++impl_->stats.request_connects_received;
                    if (ok) {
                        ++impl_->stats.accept_connects_sent;
                        std::fprintf(stderr,
                            "[listener] (TAH) RequestConnect from %s:%u -> slot %u, "
                            "replied AcceptConnect (nonce %02x%02x%02x sep=%02x parity=%02x)\n",
                            peer.host.c_str(), peer.port, sess->player_slot,
                            sess->nonce[0], sess->nonce[1], sess->nonce[2],
                            separator, req_parity);
                        // 14c-I-pcap-diff §3 — the public TAH server emits
                        // NO server_info ("SINF") packet immediately after
                        // AC. Mission name publish belongs in the scope-
                        // always burst per TRIBES-INITIAL-BURST.md §1.1.
                    } else {
                        impl_->last_error = impl_->socket.last_error();
                        std::fprintf(stderr,
                            "[listener] (TAH) send_to %s:%u failed: %s\n",
                            peer.host.c_str(), peer.port, impl_->last_error.c_str());
                    }
                }
            }
            else if (looks_like_first_data_packet(buf)) {
                Session* sess = impl_->sessions->find(peer);
                if (!sess) {
                    // DataPacket from a peer we don't have a session for —
                    // either a leftover from a reaped session or a stray
                    // packet. Log + drop.
                    std::lock_guard<std::mutex> lk(impl_->mu);
                    ++impl_->stats.unknown_packets_received;
                    std::fprintf(stderr,
                        "[listener] DataPacket from %s:%u with no session — dropped\n",
                        peer.host.c_str(), peer.port);
                    continue;
                }
                impl_->sessions->touch(peer, now_ms);
                // 14c-PhaseA: for TAH sessions, the 4-byte pure-ack
                // (07 08 09 80) is the connection-layer ack of our
                // AcceptConnect (spec §7.3). It is NOT the cue for the
                // Phase 1 / catalogue burst — that's the ClientReady
                // SetCLInfo DataPacket (§7.4), handled in the
                // non-movecmd branch below. Vanilla sessions still get
                // the captured ghost-burst on the first data packet for
                // back-compat.
                const bool should_send_vanilla =
                    cfg_.enable_canned_burst
                    && !sess->ghost_burst_sent
                    && !sess->is_tah_session;
                if (should_send_vanilla) {
                    sess->ghost_burst_sent = true;
                    std::size_t total_sent = 0;
                    std::size_t pkt_count = 0;
                    if (impl_->socket.send_to(peer, kFirstGhostBurstTemplate,
                                              sizeof(kFirstGhostBurstTemplate))) {
                        total_sent = sizeof(kFirstGhostBurstTemplate);
                        pkt_count = 1;
                    }
                    if (pkt_count > 0) sess->last_outbound_ms = now_ms;
                    std::lock_guard<std::mutex> lk(impl_->mu);
                    ++impl_->stats.data_packets_received;
                    if (pkt_count > 0) {
                        impl_->stats.ghost_bursts_sent += pkt_count;
                        std::fprintf(stderr,
                            "[listener] DataPacket from %s:%u (slot %u, vanilla), replied ghost burst (%zu pkts / %zuB)\n",
                            peer.host.c_str(), peer.port, sess->player_slot,
                            pkt_count, total_sent);
                    } else {
                        impl_->last_error = impl_->socket.last_error();
                    }
                } else {
                    std::lock_guard<std::mutex> lk(impl_->mu);
                    ++impl_->stats.data_packets_received;
                    // Quiet: subsequent DataPackets are normal client
                    // acks; don't log every one.
                }
            }
            else {
                // Spec 28/02: try to decode as a movecommand DataPacket.
                // We only attempt this when there's a live session for the
                // peer; otherwise it's stray traffic.
                Session* sess = impl_->sessions->find(peer);
                net20::MoveCommandInputs mc{};
                const bool ok = sess != nullptr
                    && buf.size() >= 8
                    && net20::decode_movecommand(buf.data(), buf.size(), mc);
                if (ok) {
                    impl_->sessions->touch(peer, now_ms);
                    {
                        std::lock_guard<std::mutex> lk(impl_->mu);
                        impl_->stats.movecommands_received += mc.moves.size();
                    }
                    // Push moves onto session queue, capped at 64 to bound
                    // memory if a misbehaving client floods us.
                    std::uint32_t seq = mc.first_move_seq;
                    for (const auto& m : mc.moves) {
                        if (seq > sess->last_applied_move_seq) {
                            sess->pending_moves.push_back(m);
                            if (sess->pending_moves.size() > 64) {
                                sess->pending_moves.pop_front();
                            }
                        }
                        ++seq;
                    }
                } else if (sess) {
                    // 26/10b follow-up — TAH's connection-progression
                    // events (and any other non-movecommand DataPacket
                    // shape from an established session) should keep
                    // the session alive AND, if we haven't sent the
                    // canned burst yet, trigger it. Otherwise TAH
                    // stays in "waiting for ghost burst" forever and
                    // we time out.
                    impl_->sessions->touch(peer, now_ms);
                    // 14c-PhaseA P1+P2: detect the TAH ClientReady packet
                    // (carries a SimConsoleEvent "SetCLInfo") and reply
                    // with the spec §7.5 Phase 1 packet (TeamAdds +
                    // PlayerAdd + SVInfo + MODInfo) followed by the spec
                    // §4 full catalogue burst ending in IrcChannelData
                    // sentinel — which is what triggers TAH's
                    // dataFinished and the load-screen-completion path.
                    const bool is_tah_clientready =
                        sess->is_tah_session
                        && !sess->ghost_burst_sent
                        && cfg_.enable_canned_burst
                        && is_setclinfo_clientready(buf);
                    bool should_send = cfg_.enable_canned_burst
                        && !sess->ghost_burst_sent
                        && !sess->is_tah_session;  // vanilla canned-burst path
                    if (is_tah_clientready) {
                        sess->ghost_burst_sent = true;
                        std::size_t total_sent = 0;
                        std::size_t pkt_count = 0;
                        // Phase 1 reply.
                        auto p1 = build_phase1_reply(*sess, now_ms);
                        if (impl_->socket.send_to(peer, p1.data(), p1.size())) {
                            total_sent += p1.size();
                            ++pkt_count;
                        }
                        // Full catalogue burst.
                        auto cat = build_catalogue_burst(*sess, now_ms);
                        for (const auto& p : cat) {
                            if (impl_->socket.send_to(peer, p.data(), p.size())) {
                                total_sent += p.size();
                                ++pkt_count;
                            }
                        }
                        if (pkt_count > 0) sess->last_outbound_ms = now_ms;
                        std::lock_guard<std::mutex> lk(impl_->mu);
                        ++impl_->stats.data_packets_received;
                        if (pkt_count > 0) {
                            impl_->stats.ghost_bursts_sent += pkt_count;
                            std::fprintf(stderr,
                                "[listener] TAH ClientReady from %s:%u (slot %u): replied phase1+catalogue (%zu pkts / %zuB)\n",
                                peer.host.c_str(), peer.port,
                                sess->player_slot, pkt_count, total_sent);
                        }
                    } else if (should_send) {
                        sess->ghost_burst_sent = true;
                        std::size_t total_sent = 0;
                        std::size_t pkt_count = 0;
                        if (impl_->socket.send_to(peer, kFirstGhostBurstTemplate,
                                                  sizeof(kFirstGhostBurstTemplate))) {
                            total_sent = sizeof(kFirstGhostBurstTemplate);
                            pkt_count = 1;
                        }
                        if (pkt_count > 0) sess->last_outbound_ms = now_ms;
                        std::lock_guard<std::mutex> lk(impl_->mu);
                        ++impl_->stats.data_packets_received;
                        if (pkt_count > 0) {
                            impl_->stats.ghost_bursts_sent += pkt_count;
                            std::fprintf(stderr,
                                "[listener] non-movecmd DataPacket %zuB from %s:%u (slot %u, vanilla), replied ghost burst (%zu pkts / %zuB)\n",
                                buf.size(), peer.host.c_str(), peer.port,
                                sess->player_slot,
                                pkt_count, total_sent);
                        }
                    } else {
                        // Burst already sent — treat unknown shapes as
                        // session keep-alive (TAH alternates several
                        // pure-ack flavours). Touch but log once per
                        // distinct prefix to avoid spam.
                        impl_->sessions->touch(peer, now_ms);
                        std::lock_guard<std::mutex> lk(impl_->mu);
                        ++impl_->stats.malformed_movecommands;
                        // Suppress repeated identical lines: only log
                        // when the byte-3 value differs from the last
                        // seen (rough but good enough to spot novel
                        // packet shapes).
                        const std::uint8_t b3 = buf.size() >= 4 ? buf[3] : 0;
                        if (b3 != impl_->last_keepalive_byte3) {
                            impl_->last_keepalive_byte3 = b3;
                            char hexbuf[256] = {};
                            hex_prefix(buf, hexbuf, sizeof(hexbuf));
                            std::fprintf(stderr,
                                "[listener] keep-alive %zuB from %s:%u (slot %u): %s\n",
                                buf.size(), peer.host.c_str(), peer.port,
                                sess->player_slot, hexbuf);
                        }
                    }
                } else {
                    char hexbuf[256] = {};
                    hex_prefix(buf, hexbuf, sizeof(hexbuf));
                    std::lock_guard<std::mutex> lk(impl_->mu);
                    ++impl_->stats.unknown_packets_received;
                    std::fprintf(stderr,
                        "[listener] unknown %zuB from %s:%u: %s\n",
                        buf.size(), peer.host.c_str(), peer.port, hexbuf);
                }
            }
        }

        // Reap stale sessions once per tick.
        std::vector<Session> dropped;
        const std::size_t n_dropped = impl_->sessions->reap(
            now_ms, cfg_.session_timeout_ms, &dropped);
        if (n_dropped > 0) {
            std::lock_guard<std::mutex> lk(impl_->mu);
            impl_->stats.sessions_dropped += n_dropped;
            for (const auto& s : dropped) {
                std::fprintf(stderr,
                    "[listener] session timeout for slot %u (%s:%u)\n",
                    s.player_slot, s.peer.host.c_str(), s.peer.port);
            }
        }

        // 26/14a — per-tick pure-ack tick. For each active session,
        // emit a 4-byte pure-ack if EITHER (a) 12+ acks are pending
        // (§14.5 force-emit rule), OR (b) 250ms have elapsed since
        // our last outbound to this peer. This keeps TAH from
        // retransmitting its own packets thinking the server is dead.
        for (auto* s : impl_->sessions->active_sessions()) {
            if (!s) continue;
            const bool force   = s->ack.should_force_ack();
            const bool quiet   = (now_ms - s->last_outbound_ms) > 250;
            const bool pending = s->ack.pending_count() > 0;
            if (!(force || (quiet && pending))) continue;

            net20::VcHeaderInputs hdr;
            hdr.send_seq               = s->next_send_seq;
            hdr.connect_parity         = s->connect_parity;
            hdr.highest_acked_of_mine  = s->ack.highest_recv_mod32;
            hdr.ack_runs               = net20::build_ack_runs(
                s->ack.received, s->ack.highest_recv_mod32);
            hdr.type_word              = net20::pkt_type::kPureAck;
            const auto wire            = net20::encode_vc_header(hdr);
            if (impl_->socket.send_to(s->peer, wire.data(), wire.size())) {
                s->ack.clear_pending();
                s->ack.total_acks_sent += 1;
                s->last_outbound_ms     = now_ms;
                // pure-ack doesn't bump send_seq (§14.2)
            }
        }

        // Spec 28/04 — per-tick OSGB emission. Throttle to every Nth
        // tick so 32 Hz tick → ~16 Hz net (close to T1's 30 Hz).
        ++impl_->tick_counter;
        if (cfg_.enable_ghost_emit
            && cfg_.ghost_emit_tick_div > 0
            && (impl_->tick_counter % static_cast<std::uint64_t>(cfg_.ghost_emit_tick_div)) == 0)
        {
            auto active = impl_->sessions->active_sessions();
            // 14c-I-osgb-gate-revert — public-server pcap evidence
            // (tah-to-public-2026-06-16.pcap) shows the public TAH
            // server streams ~15 Hz of OSGB packets continuously
            // starting immediately after the burst — it does NOT wait
            // for the burst to be acked. The earlier burst-ack gate
            // stalled OSGB indefinitely in TAH's "waiting for stream"
            // state. The stale-peer filter survives: it killed the
            // rogue port-52466 emission observed in
            // our-burst-trim-2026-06-16.pcap when the reaper hadn't
            // dropped a quiet session yet.
            std::vector<Session*> emit_targets;
            emit_targets.reserve(active.size());
            for (auto* s : active) {
                if (!s) continue;
                if (now_ms - s->last_seen_ms > 5000) continue;
                emit_targets.push_back(s);
            }
            // Drop emitters whose session no longer exists OR is now
            // stale. (Skipping emit for a session keeps the emitter
            // in the map otherwise; we want it freed so a post-quiet
            // recovery starts clean.)
            for (auto it = impl_->emitters.begin(); it != impl_->emitters.end(); ) {
                bool alive = false;
                for (auto* s : emit_targets) {
                    if (s && s->peer.host == it->first.host
                        && s->peer.port == it->first.port) {
                        alive = true; break;
                    }
                }
                if (!alive) it = impl_->emitters.erase(it);
                else ++it;
            }
            // Build the snapshot once; share by const-ref with every emitter.
            // Snapshot carries the FULL active set as "world" — even a
            // stale peer can still appear as a ghost in OTHER peers'
            // emissions; we only suppress the OUTBOUND emit for stale
            // peers (handled by emit_targets above).
            ServerWorldSnapshot world;
            world.tick = impl_->tick_counter;
            world.server_time_ms = now_ms;
            world.players.reserve(active.size());
            for (auto* s : active) world.players.push_back(s);
            // Sink closure — captures `this` so we can route through the
            // listener's socket + stats counters.
            auto sink = [this](const Endpoint& dst, const std::uint8_t* data,
                               std::size_t size) -> bool {
                const bool ok = impl_->socket.send_to(dst, data, size);
                if (ok) {
                    std::lock_guard<std::mutex> lk(impl_->mu);
                    impl_->stats.ghost_emit_packets += 1;
                    impl_->stats.ghost_emit_bytes   += size;
                } else {
                    std::lock_guard<std::mutex> lk(impl_->mu);
                    impl_->last_error = impl_->socket.last_error();
                }
                return ok;
            };
            std::uint64_t records_before_total = 0;
            for (auto& kv : impl_->emitters) {
                records_before_total += kv.second->stats().records_emitted;
            }
            for (auto* s : emit_targets) {
                EmitterKey key{s->peer.host, s->peer.port};
                auto it = impl_->emitters.find(key);
                if (it == impl_->emitters.end()) {
                    it = impl_->emitters.emplace(
                        key, std::make_unique<GhostEmitter>(s, sink)).first;
                }
                it->second->emit(world);
            }
            std::uint64_t records_after_total = 0;
            for (auto& kv : impl_->emitters) {
                records_after_total += kv.second->stats().records_emitted;
            }
            const std::uint64_t delta = records_after_total - records_before_total;
            if (delta > 0) {
                std::lock_guard<std::mutex> lk(impl_->mu);
                impl_->stats.ghost_emit_records += delta;
            }
        }

        // Publish live session count.
        {
            std::lock_guard<std::mutex> lk(impl_->mu);
            impl_->stats.sessions_active = impl_->sessions->size();
        }

        std::this_thread::sleep_for(period - (steady_clock::now() - t0));
    }
}

// R-8 shape catalogue selftest — runs the universal classifier
// against the captured RequestConnect vector for each known shape and
// verifies the derived parity/nonce offsets match the universal formula
// (parity@total-43, nonce@total-42). All 8 vectors are verbatim from
// /tmp/server-i6b/i7/i7d/i7d2/i7d3.log on the VM (sessions 2026-05 to
// 2026-06-16). Each pair (lo/hi) is the same packet with parity byte
// toggled 0x18 ↔ 0x58.
static int run_tah_shape_catalogue_selftest()
{
    struct ShapeVec {
        const char* name;
        std::vector<std::uint8_t> bytes;
        std::size_t expect_parity_off;
        std::size_t expect_nonce_off;
    };
    const std::vector<ShapeVec> vecs = {
        // Shape D 52B — sessions i7 / i7d (0x07 / 0x05 leading).
        { "D-lo-07", {
            0x07,0x00,0x11,0x23,0x47,0x83,0xa7,0xe3,0x01,0x18,0xf9,0x93,0x48,0x1c,
            0x01,0x00,0x0d,0x56,0xc6,0xbb,0x6f,0x09,0xc4,0x32,0x1a,0x11,0x40,0x04,
            0x69,0xa8,0x93,0x1e,0x3c,0x13,0x78,0x3b,0xe5,0xe3,0x0b,0x50,0x8e,0x17,
            0x37,0xc4,0x09,0x18,0xf8,0x5b,0x64,0xdf,0x65,0x7b }, 9, 10 },
        { "D-hi-07", {
            0x07,0x00,0x11,0x23,0x47,0x83,0xa7,0xe3,0x01,0x58,0xf9,0x93,0x48,0x1c,
            0x01,0x00,0x0d,0x56,0xc6,0xbb,0x6f,0x09,0xc4,0x32,0x1a,0x11,0x40,0x04,
            0x69,0xa8,0x93,0x1e,0x3c,0x13,0x78,0x3b,0xe5,0xe3,0x0b,0x50,0x8e,0x17,
            0x37,0xc4,0x09,0x18,0xf8,0x5b,0x64,0xdf,0x65,0x7b }, 9, 10 },
        // Shape E 46B — session i7d2 (0x07 leading).
        { "E-lo-07", {
            0x07,0x00,0x69,0x18,0x49,0xba,0x04,0x1d,
            0x01,0x00,0x0d,0x56,0xc6,0xbb,0x6f,0x09,0xc4,0x32,0x1a,0x11,0x40,0x04,
            0x69,0xa8,0x93,0x1e,0x3c,0x13,0x78,0x3b,0xe5,0xe3,0x0b,0x50,0x8e,0x17,
            0x37,0xc4,0x09,0x18,0xf8,0x5b,0x64,0xdf,0x65,0x7b }, 3, 4 },
        // Shape E' 46B — session i7d3 (0x05 leading variant).
        { "E'-hi-05", {
            0x05,0x00,0x09,0x58,0xec,0x02,0x0b,0x1d,
            0x01,0x00,0x0d,0x56,0xc6,0xbb,0x6f,0x09,0xc4,0x32,0x1a,0x11,0x40,0x04,
            0x69,0xa8,0x93,0x1e,0x3c,0x13,0x78,0x3b,0xe5,0xe3,0x0b,0x50,0x8e,0x17,
            0x37,0xc4,0x09,0x18,0xf8,0x5b,0x64,0xdf,0x65,0x7b }, 3, 4 },
        // Shape F 50B — session i7d3.
        { "F-lo-05", {
            0x05,0x00,0x17,0x4f,0x87,0xbf,0xf3,0x18,0x2c,0xa0,0x0b,0x1d,
            0x01,0x00,0x0d,0x56,0xc6,0xbb,0x6f,0x09,0xc4,0x32,0x1a,0x11,0x40,0x04,
            0x69,0xa8,0x93,0x1e,0x3c,0x13,0x78,0x3b,0xe5,0xe3,0x0b,0x50,0x8e,0x17,
            0x37,0xc4,0x09,0x18,0xf8,0x5b,0x64,0xdf,0x65,0x7b }, 7, 8 },
        // Shape F 50B — earlier session i6b (0x07 leading, different nonce).
        { "F-lo-07-i6b", {
            0x07,0x00,0x17,0x4f,0x87,0xbf,0xf3,0x18,0x71,0x1d,0xe0,0x19,
            0x01,0x00,0x0d,0x56,0xc6,0xbb,0x6f,0x09,0xc4,0x32,0x1a,0x11,0x40,0x04,
            0x69,0xa8,0x93,0x1e,0x3c,0x13,0x78,0x3b,0xe5,0xe3,0x0b,0x50,0x8e,0x17,
            0x37,0xc4,0x09,0x18,0xf8,0x5b,0x64,0xdf,0x65,0x7b }, 7, 8 },
        // Shape G 54B — session i7d3.
        { "G-lo-07", {
            0x07,0x00,0x11,0x27,0x59,0x71,0x82,0xb1,0xc2,0xe2,0x01,0x18,0x2d,0xa0,
            0x0b,0x1d,
            0x01,0x00,0x0d,0x56,0xc6,0xbb,0x6f,0x09,0xc4,0x32,0x1a,0x11,0x40,0x04,
            0x69,0xa8,0x93,0x1e,0x3c,0x13,0x78,0x3b,0xe5,0xe3,0x0b,0x50,0x8e,0x17,
            0x37,0xc4,0x09,0x18,0xf8,0x5b,0x64,0xdf,0x65,0x7b }, 11, 12 },
        { "G-hi-07", {
            0x07,0x00,0x11,0x27,0x59,0x71,0x82,0xb1,0xc2,0xe2,0x01,0x58,0x2d,0xa0,
            0x0b,0x1d,
            0x01,0x00,0x0d,0x56,0xc6,0xbb,0x6f,0x09,0xc4,0x32,0x1a,0x11,0x40,0x04,
            0x69,0xa8,0x93,0x1e,0x3c,0x13,0x78,0x3b,0xe5,0xe3,0x0b,0x50,0x8e,0x17,
            0x37,0xc4,0x09,0x18,0xf8,0x5b,0x64,0xdf,0x65,0x7b }, 11, 12 },
    };
    int failures = 0;
    for (const auto& v : vecs) {
        std::size_t po = 0, no = 0;
        const bool ok = looks_like_tah_request_connect(v.bytes, &po, &no);
        if (!ok) {
            std::fprintf(stderr,
                "[r8-selftest] FAIL %s: classifier rejected\n", v.name);
            ++failures;
            continue;
        }
        if (po != v.expect_parity_off || no != v.expect_nonce_off) {
            std::fprintf(stderr,
                "[r8-selftest] FAIL %s: got parity@%zu nonce@%zu, "
                "want parity@%zu nonce@%zu\n",
                v.name, po, no, v.expect_parity_off, v.expect_nonce_off);
            ++failures;
        }
    }
    // Negative tests: random 45B Groove-sized + 50B with bad bytes 0..1.
    {
        std::vector<std::uint8_t> not_tah(50, 0xaa);
        not_tah[0] = 0x05; not_tah[1] = 0x00; not_tah[7] = 0x18; not_tah[11] = 0x1d;
        std::size_t po = 0, no = 0;
        // This SHOULD pass — it's structurally TAH. Negative test instead:
        std::vector<std::uint8_t> bad(50, 0xaa);
        bad[0] = 0x03;  // not 0x05/0x07
        if (looks_like_tah_request_connect(bad, &po, &no)) {
            std::fprintf(stderr,
                "[r8-selftest] FAIL: classifier accepted byte0=0x03\n");
            ++failures;
        }
    }
    if (failures == 0) {
        std::fprintf(stderr,
            "[r8-selftest] OK — universal classifier accepted "
            "%zu shape vectors (D/E/E'/F×2/G×2)\n", vecs.size());
    }
    return failures;
}

int server_listener_selftest()
{
    using studio::content::net::Endpoint;
    using studio::content::net::UdpSocket;

    // R-8 — universal TAH RequestConnect classifier coverage test.
    if (int rc = run_tah_shape_catalogue_selftest(); rc != 0) {
        std::fprintf(stderr,
            "[listener-selftest] R-8 catalogue selftest failed (%d errors)\n", rc);
        return 1;
    }

    // Bring up the listener on an ephemeral port.
    UdpSocket probe;
    if (!probe.bind(0)) {
        std::fprintf(stderr, "[listener-selftest] probe bind failed: %s\n",
                     probe.last_error().c_str());
        return 1;
    }

    // Pick a deterministic listener port: probe's local port + 1, hoping
    // it's free. (For a more robust test we'd bind to 0 and read back
    // local_port, but ServerListenerConfig.port is meant to be explicit.)
    std::uint16_t listener_port = 0;
    UdpSocket bind_probe;
    if (!bind_probe.bind(0)) {
        std::fprintf(stderr, "[listener-selftest] bind_probe failed: %s\n",
                     bind_probe.last_error().c_str());
        return 1;
    }
    listener_port = bind_probe.local_port();
    bind_probe.close();  // release so ServerListener can grab it

    ServerListenerConfig cfg1{};
    cfg1.port = listener_port;
    cfg1.tick_hz = 200;
    cfg1.enable_ghost_emit = false;  // phases 1-5 test handshake / canned burst / moves
    ServerListener listener(cfg1);
    if (!listener.start()) {
        std::fprintf(stderr, "[listener-selftest] start failed: %s\n",
                     listener.last_error().c_str());
        return 1;
    }

    // Build a synthetic RequestConnect with a known nonce.
    std::uint8_t request[27] = {
        0x07, 0x00, 0x13, 0x44, 0xa7, 0xe5, 0x18,
        0xde, 0xad, 0xbe,                            // <- nonce
        0x12, 0x01, 0x00, 0x0d, 0x56, 0xc6, 0xbb, 0x6f, 0x07, 0xc4, 0x52,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };

    Endpoint target{"127.0.0.1", listener_port};
    if (!probe.send_to(target, request, sizeof(request))) {
        std::fprintf(stderr, "[listener-selftest] probe.send_to failed: %s\n",
                     probe.last_error().c_str());
        listener.stop();
        return 1;
    }

    // Poll for the reply up to ~500ms.
    std::vector<std::uint8_t> reply_buf;
    Endpoint reply_peer;
    for (int i = 0; i < 100; ++i) {
        if (probe.try_recv(reply_buf, reply_peer)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    listener.stop();

    if (reply_buf.size() != 16) {
        std::fprintf(stderr, "[listener-selftest] expected 16-byte reply, got %zu\n",
                     reply_buf.size());
        return 1;
    }
    std::uint8_t expected[16];
    const std::uint8_t nonce[3] = { 0xde, 0xad, 0xbe };
    build_accept_connect_reply(nonce, expected);
    if (std::memcmp(reply_buf.data(), expected, 16) != 0) {
        std::fprintf(stderr, "[listener-selftest] reply bytes mismatch\n");
        return 1;
    }

    // Phase 2: send the first DataPacket (07 08 09 80) and expect the
    // 223-byte ghost burst per spec 26/11. Listener already stopped
    // above — restart for the second leg with a fresh ephemeral port.
    UdpSocket bind_probe2;
    if (!bind_probe2.bind(0)) {
        std::fprintf(stderr, "[listener-selftest] phase2 bind_probe failed\n");
        return 1;
    }
    listener_port = bind_probe2.local_port();
    bind_probe2.close();
    ServerListenerConfig cfg2{};
    cfg2.port = listener_port;
    cfg2.tick_hz = 200;
    cfg2.enable_ghost_emit = false;
    ServerListener listener2(cfg2);
    if (!listener2.start()) {
        std::fprintf(stderr, "[listener-selftest] phase2 listener.start failed: %s\n",
                     listener2.last_error().c_str());
        return 1;
    }
    Endpoint target2{"127.0.0.1", listener_port};
    // Phase 2 now requires an established session (spec 28/01 gates
    // DataPacket -> burst on session.ghost_burst_sent). Send a
    // RequestConnect first, swallow the AcceptConnect, then send the
    // DataPacket and expect the burst.
    std::uint8_t request2[27] = {
        0x07, 0x00, 0x13, 0x44, 0xa7, 0xe5, 0x18,
        0xca, 0xfe, 0xba,
        0x12, 0x01, 0x00, 0x0d, 0x56, 0xc6, 0xbb, 0x6f, 0x07, 0xc4, 0x52,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    probe.send_to(target2, request2, sizeof(request2));
    reply_buf.clear();
    for (int i = 0; i < 100; ++i) {
        if (probe.try_recv(reply_buf, reply_peer)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (reply_buf.size() != 16) {
        std::fprintf(stderr,
            "[listener-selftest] phase2 pre-handshake AcceptConnect missing (size=%zu)\n",
            reply_buf.size());
        listener2.stop();
        return 1;
    }
    const std::uint8_t data_packet[4] = { 0x07, 0x08, 0x09, 0x80 };
    if (!probe.send_to(target2, data_packet, sizeof(data_packet))) {
        std::fprintf(stderr, "[listener-selftest] phase2 probe.send_to failed\n");
        listener2.stop();
        return 1;
    }
    reply_buf.clear();
    for (int i = 0; i < 200; ++i) {
        if (probe.try_recv(reply_buf, reply_peer)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    listener2.stop();
    if (reply_buf.size() != 223) {
        std::fprintf(stderr,
            "[listener-selftest] phase2: expected 223-byte ghost burst, got %zu\n",
            reply_buf.size());
        return 1;
    }
    if (reply_buf[0] != 0x0b) {
        std::fprintf(stderr,
            "[listener-selftest] phase2: ghost burst byte 0 expected 0x0b, got 0x%02x\n",
            reply_buf[0]);
        return 1;
    }

    // Phase 3 (spec 28/01): three RequestConnects from three ports must
    // all get AcceptConnect; second DataPacket from the same peer must
    // NOT trigger a second ghost burst.
    UdpSocket bind3;
    if (!bind3.bind(0)) {
        std::fprintf(stderr, "[listener-selftest] phase3 bind failed\n");
        return 1;
    }
    listener_port = bind3.local_port();
    bind3.close();
    ServerListenerConfig cfg3{};
    cfg3.port = listener_port;
    cfg3.tick_hz = 200;
    cfg3.max_players = 8;
    cfg3.enable_ghost_emit = false;
    ServerListener listener3(cfg3);
    if (!listener3.start()) {
        std::fprintf(stderr, "[listener-selftest] phase3 start failed: %s\n",
                     listener3.last_error().c_str());
        return 1;
    }
    Endpoint target3{"127.0.0.1", listener_port};

    UdpSocket clients[3];
    for (int i = 0; i < 3; ++i) {
        if (!clients[i].bind(0)) {
            std::fprintf(stderr, "[listener-selftest] phase3 client %d bind failed\n", i);
            listener3.stop();
            return 1;
        }
    }
    auto send_request = [&](UdpSocket& sock, std::uint8_t tag) {
        std::uint8_t req[27] = {
            0x07, 0x00, 0x13, 0x44, 0xa7, 0xe5, 0x18,
            tag, std::uint8_t(tag + 1), std::uint8_t(tag + 2),
            0x12, 0x01, 0x00, 0x0d, 0x56, 0xc6, 0xbb, 0x6f, 0x07, 0xc4, 0x52,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        return sock.send_to(target3, req, sizeof(req));
    };
    auto recv_with_timeout = [](UdpSocket& sock, std::vector<std::uint8_t>& out, int ms_total) {
        Endpoint p;
        const int iters = ms_total / 5;
        for (int i = 0; i < iters; ++i) {
            if (sock.try_recv(out, p)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };

    for (int i = 0; i < 3; ++i) {
        if (!send_request(clients[i], std::uint8_t(0x10 + i*0x10))) {
            std::fprintf(stderr, "[listener-selftest] phase3 send_request %d failed\n", i);
            listener3.stop();
            return 1;
        }
        std::vector<std::uint8_t> rbuf;
        if (!recv_with_timeout(clients[i], rbuf, 500) || rbuf.size() != 16) {
            std::fprintf(stderr,
                "[listener-selftest] phase3 client %d: missing/short AcceptConnect (size=%zu)\n",
                i, rbuf.size());
            listener3.stop();
            return 1;
        }
    }

    // Each client sends a DataPacket; expect one ghost burst per peer.
    const std::uint8_t dp[4] = { 0x07, 0x08, 0x09, 0x80 };
    for (int i = 0; i < 3; ++i) {
        clients[i].send_to(target3, dp, 4);
        std::vector<std::uint8_t> rbuf;
        if (!recv_with_timeout(clients[i], rbuf, 500) || rbuf.size() != 223) {
            std::fprintf(stderr,
                "[listener-selftest] phase3 client %d: missing/short ghost burst\n", i);
            listener3.stop();
            return 1;
        }
    }
    // Second DataPacket from client 0 must NOT yield another burst.
    // 14a follow-up: pure-acks (4B) from the periodic 250ms tick are
    // expected and not a dedup failure; only a 2nd burst (>= 18B) is.
    clients[0].send_to(target3, dp, 4);
    std::vector<std::uint8_t> rbuf_dup;
    while (recv_with_timeout(clients[0], rbuf_dup, 200)) {
        if (rbuf_dup.size() >= 18) {
            std::fprintf(stderr,
                "[listener-selftest] phase3: duplicate DataPacket triggered another reply (%zuB) — dedup broken\n",
                rbuf_dup.size());
            listener3.stop();
            return 1;
        }
        // size < 18 is a pure-ack from the 14a tick; ignore and keep looking.
    }
    listener3.stop();

    // Phase 4: max_players=2; third RequestConnect gets RejectConnect.
    UdpSocket bind4;
    if (!bind4.bind(0)) return 1;
    listener_port = bind4.local_port();
    bind4.close();
    ServerListenerConfig cfg4{};
    cfg4.port = listener_port;
    cfg4.tick_hz = 200;
    cfg4.max_players = 2;
    cfg4.enable_ghost_emit = false;
    ServerListener listener4(cfg4);
    if (!listener4.start()) {
        std::fprintf(stderr, "[listener-selftest] phase4 start failed\n");
        return 1;
    }
    Endpoint target4{"127.0.0.1", listener_port};
    UdpSocket cl4[3];
    for (int i = 0; i < 3; ++i) cl4[i].bind(0);
    for (int i = 0; i < 3; ++i) {
        std::uint8_t req[27] = {
            0x07, 0x00, 0x13, 0x44, 0xa7, 0xe5, 0x18,
            std::uint8_t(0xaa + i), 0xbb, 0xcc,
            0x12, 0x01, 0x00, 0x0d, 0x56, 0xc6, 0xbb, 0x6f, 0x07, 0xc4, 0x52,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        cl4[i].send_to(target4, req, sizeof(req));
    }
    // Clients 0, 1 should get AcceptConnect (16B); client 2 gets
    // RejectConnect (24B with "Server is full.").
    for (int i = 0; i < 2; ++i) {
        std::vector<std::uint8_t> rbuf;
        if (!recv_with_timeout(cl4[i], rbuf, 500) || rbuf.size() != 16) {
            std::fprintf(stderr,
                "[listener-selftest] phase4 client %d: expected 16B Accept, got %zu\n",
                i, rbuf.size());
            listener4.stop();
            return 1;
        }
    }
    std::vector<std::uint8_t> rej;
    if (!recv_with_timeout(cl4[2], rej, 500) || rej.size() != 24) {
        std::fprintf(stderr,
            "[listener-selftest] phase4 client 2: expected 24B Reject, got %zu\n",
            rej.size());
        listener4.stop();
        return 1;
    }
    if (rej[2] != 0x09 || rej[3] != 0x28) {
        std::fprintf(stderr,
            "[listener-selftest] phase4: reject reply header wrong (bytes 2-3 = %02x %02x; want 09 28)\n",
            rej[2], rej[3]);
        listener4.stop();
        return 1;
    }
    if (std::memcmp(rej.data() + 8, "Server is full.", 15) != 0) {
        std::fprintf(stderr,
            "[listener-selftest] phase4: reject reason mismatch\n");
        listener4.stop();
        return 1;
    }
    listener4.stop();

    // Phase 5 (spec 28/02): movecommand round-trip + listener ingestion.
    // (a) encode a known MoveCommandInputs, decode it, assert field-equal.
    {
        net20::MoveCommandInputs src{};
        src.send_seq = 7;
        src.connect_parity = true;
        src.highest_acked_of_mine = 3;
        src.fov_degrees = 90.0f;
        src.first_move_seq = 100;
        net20::MoveInput m{};
        m.forward = 1.0f; m.left = 0.5f;
        m.jet = true; m.jump = true;
        m.trigger = true;
        m.yaw_delta = 0.125f;
        m.pitch_delta = -0.0625f;
        src.moves.push_back(m);
        m.jump = false;
        src.moves.push_back(m);
        const auto bytes = net20::encode_movecommand(src);
        net20::MoveCommandInputs dec{};
        if (!net20::decode_movecommand(bytes.data(), bytes.size(), dec)) {
            std::fprintf(stderr, "[listener-selftest] phase5: decode failed\n");
            return 1;
        }
        if (dec.send_seq != src.send_seq ||
            dec.connect_parity != src.connect_parity ||
            dec.highest_acked_of_mine != src.highest_acked_of_mine ||
            dec.first_move_seq != src.first_move_seq ||
            dec.moves.size() != src.moves.size()) {
            std::fprintf(stderr,
                "[listener-selftest] phase5: header round-trip mismatch "
                "(seq=%u/%u parity=%d/%d ha=%u/%u first=%u/%u moves=%zu/%zu)\n",
                dec.send_seq, src.send_seq,
                int(dec.connect_parity), int(src.connect_parity),
                dec.highest_acked_of_mine, src.highest_acked_of_mine,
                dec.first_move_seq, src.first_move_seq,
                dec.moves.size(), src.moves.size());
            return 1;
        }
        // The axes are quantized to 4 bits — exact equality only holds
        // when src values are themselves multiples of 1/15.
        if (dec.moves[0].jet   != src.moves[0].jet   ||
            dec.moves[0].jump  != src.moves[0].jump  ||
            dec.moves[0].trigger != src.moves[0].trigger ||
            dec.moves[0].yaw_delta   != src.moves[0].yaw_delta ||
            dec.moves[0].pitch_delta != src.moves[0].pitch_delta) {
            std::fprintf(stderr, "[listener-selftest] phase5: move fields mismatch\n");
            return 1;
        }
    }

    // (b) live ingest test: bring up a listener; handshake one peer;
    // send a movecommand packet; assert session has 2 pending moves.
    {
        UdpSocket bind5;
        if (!bind5.bind(0)) return 1;
        const std::uint16_t lp = bind5.local_port();
        bind5.close();
        ServerListenerConfig cfg5{};
        cfg5.port = lp;
        cfg5.tick_hz = 200;
        cfg5.max_players = 4;
        cfg5.enable_ghost_emit = false;
        ServerListener l5(cfg5);
        if (!l5.start()) {
            std::fprintf(stderr, "[listener-selftest] phase5b start failed\n");
            return 1;
        }
        UdpSocket cli;
        cli.bind(0);
        Endpoint tgt{"127.0.0.1", lp};
        std::uint8_t req[27] = {
            0x07, 0x00, 0x13, 0x44, 0xa7, 0xe5, 0x18,
            0xde, 0xad, 0xbe,
            0x12, 0x01, 0x00, 0x0d, 0x56, 0xc6, 0xbb, 0x6f, 0x07, 0xc4, 0x52,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        cli.send_to(tgt, req, sizeof(req));
        // Drain AcceptConnect.
        std::vector<std::uint8_t> rbuf;
        Endpoint rp;
        for (int i = 0; i < 100; ++i) {
            if (cli.try_recv(rbuf, rp)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (rbuf.size() != 16) {
            std::fprintf(stderr, "[listener-selftest] phase5b: no AcceptConnect\n");
            l5.stop();
            return 1;
        }
        // Now send a movecommand packet with 2 moves.
        net20::MoveCommandInputs mc{};
        mc.send_seq = 1;
        mc.connect_parity = false;
        mc.highest_acked_of_mine = 1;
        mc.first_move_seq = 1;
        net20::MoveInput m{};
        m.forward = 1.0f;
        mc.moves.push_back(m);
        m.left = 1.0f;
        mc.moves.push_back(m);
        auto bytes = net20::encode_movecommand(mc);
        cli.send_to(tgt, bytes.data(), bytes.size());
        // Give the listener thread a few cycles to ingest.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto s = l5.stats();
        l5.stop();
        if (s.movecommands_received != 2) {
            std::fprintf(stderr,
                "[listener-selftest] phase5b: expected 2 movecommands_received, got %llu\n",
                (unsigned long long)s.movecommands_received);
            return 1;
        }
        if (s.malformed_movecommands != 0) {
            std::fprintf(stderr,
                "[listener-selftest] phase5b: %llu malformed (expected 0)\n",
                (unsigned long long)s.malformed_movecommands);
            return 1;
        }
    }

    std::fprintf(stderr,
        "[listener-selftest] OK — handshake + ghost burst + sessions + rejects + movecommands\n");
    return 0;
}

int groove_handshake_selftest()
{
    using studio::content::net::Endpoint;
    using studio::content::net::UdpSocket;

    UdpSocket probe;
    if (!probe.bind(0)) {
        std::fprintf(stderr, "[groove-selftest] probe bind failed: %s\n",
                     probe.last_error().c_str());
        return 1;
    }
    UdpSocket bind_probe;
    if (!bind_probe.bind(0)) {
        std::fprintf(stderr, "[groove-selftest] bind_probe failed\n");
        return 1;
    }
    const std::uint16_t listener_port = bind_probe.local_port();
    bind_probe.close();

    ServerListenerConfig cfg{};
    cfg.port = listener_port;
    cfg.tick_hz = 200;
    cfg.enable_ghost_emit = false;
    ServerListener listener(cfg);
    if (!listener.start()) {
        std::fprintf(stderr, "[groove-selftest] listener.start failed: %s\n",
                     listener.last_error().c_str());
        return 1;
    }

    // Captured Groove RequestConnect bytes (see kGrooveAcceptConnectTemplate
    // comment for source). Nonce slot at offsets 10..12 is overwritten
    // with a known sentinel so we can verify the reply echo.
    std::uint8_t request[45] = {
        0x07, 0x00, 0x12, 0x41, 0x81, 0xa1, 0xb1, 0xc7, 0xfa, 0x18,
        0xde, 0xad, 0xbe,                        // <- nonce slot
        0x00,
        0x01, 0x00, 0x0d, 0x56, 0xc6, 0xbb,
        0x6f, 0x09, 0xc4, 0x32, 0x1a, 0x11, 0xc0, 0x03, 0x89, 0xf1,
        0x13, 0x7c, 0x02, 0x6f, 0xa7, 0x7c, 0x7c, 0x01, 0xca, 0xf1,
        0x62, 0x82, 0x5d, 0xb6, 0x07,
    };

    Endpoint target{"127.0.0.1", listener_port};
    if (!probe.send_to(target, request, sizeof(request))) {
        std::fprintf(stderr, "[groove-selftest] probe.send_to failed\n");
        listener.stop();
        return 1;
    }

    std::vector<std::uint8_t> reply_buf;
    Endpoint reply_peer;
    for (int i = 0; i < 100; ++i) {
        if (probe.try_recv(reply_buf, reply_peer)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (reply_buf.size() != 18) {
        std::fprintf(stderr, "[groove-selftest] expected 18-byte reply, got %zu\n",
                     reply_buf.size());
        listener.stop();
        return 1;
    }
    const std::uint8_t nonce[3] = { 0xde, 0xad, 0xbe };
    std::uint8_t expected[18];
    build_groove_accept_connect_reply(nonce, expected);
    if (std::memcmp(reply_buf.data(), expected, 18) != 0) {
        std::fprintf(stderr, "[groove-selftest] reply bytes mismatch\n");
        std::fprintf(stderr, "  got:      ");
        for (auto b : reply_buf) std::fprintf(stderr, "%02x ", b);
        std::fprintf(stderr, "\n  expected: ");
        for (auto b : expected) std::fprintf(stderr, "%02x ", b);
        std::fprintf(stderr, "\n");
        listener.stop();
        return 1;
    }
    if (listener.sessions().size() != 1) {
        std::fprintf(stderr, "[groove-selftest] expected 1 session, got %zu\n",
                     listener.sessions().size());
        listener.stop();
        return 1;
    }

    // Variant B (0x05 leading byte): same listener should accept it
    // too. Just sanity-check the predicate path; we don't have a fresh
    // peer slot in this listener but the second RC is a retransmit-ish
    // from the SAME peer so it should be accepted as a re-allocation.
    request[0] = 0x05;
    if (!probe.send_to(target, request, sizeof(request))) {
        std::fprintf(stderr, "[groove-selftest] variant-B send failed\n");
        listener.stop();
        return 1;
    }
    reply_buf.clear();
    for (int i = 0; i < 100; ++i) {
        if (probe.try_recv(reply_buf, reply_peer)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    listener.stop();
    if (reply_buf.size() != 18) {
        std::fprintf(stderr, "[groove-selftest] variant-B expected 18B, got %zu\n",
                     reply_buf.size());
        return 1;
    }
    if (std::memcmp(reply_buf.data(), expected, 18) != 0) {
        std::fprintf(stderr, "[groove-selftest] variant-B reply mismatch\n");
        return 1;
    }

    std::fprintf(stderr,
        "[groove-selftest] OK — both 0x07 and 0x05 variants handshake\n");
    return 0;
}

} // namespace dts_viewer
