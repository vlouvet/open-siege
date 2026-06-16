#include <osengine/server_listener.hpp>

#include <osengine/ghost_emitter.hpp>
#include <osengine/tah_burst_orchestrator.hpp>
#include <osengine/tah_ghost_burst.hpp>
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

// 18-byte TribesAfterHope AcceptConnect template — discovered 2026-05-24
// by probing a real TAH dedicated server (127.0.0.1:28001) with the
// 53-byte 0x05-leading "browser-ping" packet and capturing its reply.
// Wire shape:
//   05 00 09 60  <nonce 3B>  08 <parity> 08 00 00 00 08 00 80 01 01
// Differences vs the Groove template above:
//   * Byte 0 = 0x05 (Groove had 0x07).
//   * Byte 7 = 0x08 (Groove had 0x00).
//   * Byte 8 = parity bit derived from request byte 10:
//       request[10] == 0x18 -> reply[8] = 0x02
//       request[10] == 0x58 -> reply[8] = 0x01
//     other values: unverified; default to 0x02.
constexpr std::uint8_t kTahAcceptConnectTemplate[18] = {
    0x05, 0x00, 0x09, 0x60,
    0x00, 0x00, 0x00,                // <- nonce slot (bytes 4..6)
    0x08, 0x02,                       // <- byte 8 overwritten per request
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

// TribesAfterHope RequestConnect family. TAH cycles through multiple
// connect-handshake shapes when probing a server; all three observed
// shapes share the same logical fields (parity byte then 3-byte nonce)
// at different offsets. The server replies with the same 18B AC for
// all of them.
//
// Variants (verified by capture against TAH at 192.168.1.101):
//
//   Shape A — 53B, leading 0x05  (prefix bytes 0..9 = 05 00 21 41 61
//     81 a1 c1 e1 01; parity@10; nonce@11..13). Cross-verified against
//     real TAH dedicated server: replies with 18B AC.
//
//   Shape B — 51B, leading 0x05  (prefix bytes 0..7 = 05 00 25 61 81
//     a2 c1 e5; parity@8; nonce@9..11). Observed 2026-05-24.
//
//   Shape C — 53B, leading 0x07  (prefix bytes 0..9 = 07 00 21 41 61
//     81 a1 c1 e1 01; parity@10; nonce@11..13). Observed 2026-05-24
//     interleaved with shape B in same TAH session.
//
//   Shape D — 52B, leading 0x07  (prefix bytes 0..8 = 07 00 11 23 47
//     83 a7 e3 01; parity@9; nonce@10..12). Observed 2026-06-16 against
//     live TAH on .101. Same ack-list step pattern as Shape C but 1B
//     shorter — bit-shifted by one nibble. Offset guesses (parity@9,
//     nonce@10..12) extrapolated from Shape A/C; needs Reader cross-
//     check (14c-R-8) if TAH does not accept the resulting AC reply.
//
// out_parity_off / out_nonce_off set on match.
bool looks_like_tah_request_connect(const std::vector<std::uint8_t>& pkt,
                                    std::size_t* out_parity_off,
                                    std::size_t* out_nonce_off)
{
    static const std::uint8_t prefix_a[10] = {
        0x05, 0x00, 0x21, 0x41, 0x61, 0x81, 0xa1, 0xc1, 0xe1, 0x01,
    };
    static const std::uint8_t prefix_b[8] = {
        0x05, 0x00, 0x25, 0x61, 0x81, 0xa2, 0xc1, 0xe5,
    };
    static const std::uint8_t prefix_c[10] = {
        0x07, 0x00, 0x21, 0x41, 0x61, 0x81, 0xa1, 0xc1, 0xe1, 0x01,
    };
    static const std::uint8_t prefix_d[9] = {
        0x07, 0x00, 0x11, 0x23, 0x47, 0x83, 0xa7, 0xe3, 0x01,
    };
    if (pkt.size() == 53 && std::memcmp(pkt.data(), prefix_a, 10) == 0) {
        if (out_parity_off) *out_parity_off = 10;
        if (out_nonce_off)  *out_nonce_off  = 11;
        return true;
    }
    if (pkt.size() == 51 && std::memcmp(pkt.data(), prefix_b, 8) == 0) {
        if (out_parity_off) *out_parity_off = 8;
        if (out_nonce_off)  *out_nonce_off  = 9;
        return true;
    }
    if (pkt.size() == 53 && std::memcmp(pkt.data(), prefix_c, 10) == 0) {
        if (out_parity_off) *out_parity_off = 10;
        if (out_nonce_off)  *out_nonce_off  = 11;
        return true;
    }
    if (pkt.size() == 52 && std::memcmp(pkt.data(), prefix_d, 9) == 0) {
        if (out_parity_off) *out_parity_off = 9;
        if (out_nonce_off)  *out_nonce_off  = 10;
        return true;
    }
    return false;
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

void build_tah_accept_connect_reply(const std::uint8_t nonce[3],
                                    std::uint8_t      request_parity_byte,
                                    std::uint8_t      out[18])
{
    std::memcpy(out, kTahAcceptConnectTemplate, 18);
    out[4] = nonce[0];
    out[5] = nonce[1];
    out[6] = nonce[2];
    // Mirror the TAH server's behaviour: parity byte 8 of the AC
    // reflects request byte 10 (0x18 -> 0x02, 0x58 -> 0x01).
    if      (request_parity_byte == 0x58) out[8] = 0x01;
    else if (request_parity_byte == 0x18) out[8] = 0x02;
    else                                  out[8] = 0x02;  // default
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
            if (buf.size() >= 4 && (buf[0] & 1) && impl_->sessions->find(peer)) {
                net20::ParsedIncomingHeader ph;
                if (net20::parse_incoming_header(buf.data(), buf.size(), ph)
                    && ph.base_type != net20::pkt_type::kPing) {
                    Session* sess = impl_->sessions->find(peer);
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
                        // Spec 29/02b — publish mission/slot/team to the
                        // new client right after AcceptConnect. Skip when
                        // no mission is set (listener selftest path).
                        if (was_new && !impl_->mission_name.empty()) {
                            ServerInfo si;
                            si.mission_short_name = impl_->mission_name;
                            si.player_slot = sess->player_slot;
                            si.team_raw = static_cast<std::uint8_t>(sess->team);
                            si.server_tick = static_cast<std::uint32_t>(
                                impl_->tick_counter);
                            const auto si_bytes = encode_server_info(
                                si, sess->next_send_seq, /*parity*/ false);
                            impl_->socket.send_to(peer, si_bytes.data(),
                                                  si_bytes.size());
                            sess->next_send_seq = static_cast<std::uint16_t>(
                                (sess->next_send_seq + 1) & 0x1FFu);
                            if (sess->next_send_seq == 0) sess->next_send_seq = 1;
                        }
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
                        if (was_new && !impl_->mission_name.empty()) {
                            ServerInfo si;
                            si.mission_short_name = impl_->mission_name;
                            si.player_slot = sess->player_slot;
                            si.team_raw = static_cast<std::uint8_t>(sess->team);
                            si.server_tick = static_cast<std::uint32_t>(
                                impl_->tick_counter);
                            const auto si_bytes = encode_server_info(
                                si, sess->next_send_seq, /*parity*/ false);
                            impl_->socket.send_to(peer, si_bytes.data(),
                                                  si_bytes.size());
                            sess->next_send_seq = static_cast<std::uint16_t>(
                                (sess->next_send_seq + 1) & 0x1FFu);
                            if (sess->next_send_seq == 0) sess->next_send_seq = 1;
                        }
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
                // Spec 26/10b follow-up — TribesAfterHope connect family
                // (51B/53B, 0x05/0x07 leading). Per-shape nonce + parity
                // offsets supplied by the predicate; reply is always the
                // 18B AC with parity-byte 8 derived from the request's
                // parity byte.
                const std::uint8_t nonce[3] = {
                    buf[nonce_off], buf[nonce_off + 1], buf[nonce_off + 2],
                };
                const std::uint8_t req_parity = buf[parity_off];
                const bool was_new = impl_->sessions->find(peer) == nullptr;
                Session* sess = impl_->sessions->allocate(peer, nonce, now_ms);
                std::uint8_t tah_reply[18];
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
                    build_tah_accept_connect_reply(sess->nonce, req_parity, tah_reply);
                    sess->connect_parity = (tah_reply[0] & 0x02) != 0;
                    const bool ok = impl_->socket.send_to(
                        peer, tah_reply, sizeof(tah_reply));
                    if (ok) sess->last_outbound_ms = now_ms;
                    std::lock_guard<std::mutex> lk(impl_->mu);
                    ++impl_->stats.request_connects_received;
                    if (ok) {
                        ++impl_->stats.accept_connects_sent;
                        std::fprintf(stderr,
                            "[listener] (TAH) RequestConnect from %s:%u -> slot %u, replied AcceptConnect (nonce %02x%02x%02x parity=%02x)\n",
                            peer.host.c_str(), peer.port, sess->player_slot,
                            sess->nonce[0], sess->nonce[1], sess->nonce[2],
                            req_parity);
                        if (was_new && !impl_->mission_name.empty()) {
                            ServerInfo si;
                            si.mission_short_name = impl_->mission_name;
                            si.player_slot = sess->player_slot;
                            si.team_raw = static_cast<std::uint8_t>(sess->team);
                            si.server_tick = static_cast<std::uint32_t>(
                                impl_->tick_counter);
                            const auto si_bytes = encode_server_info(
                                si, sess->next_send_seq, /*parity*/ false);
                            impl_->socket.send_to(peer, si_bytes.data(),
                                                  si_bytes.size());
                            sess->next_send_seq = static_cast<std::uint16_t>(
                                (sess->next_send_seq + 1) & 0x1FFu);
                            if (sess->next_send_seq == 0) sess->next_send_seq = 1;
                        }
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
                bool should_send = cfg_.enable_canned_burst
                    && !sess->ghost_burst_sent;
                if (should_send) {
                    sess->ghost_burst_sent = true;
                    std::size_t total_sent = 0;
                    std::size_t pkt_count = 0;
                    if (sess->is_tah_session) {
                        // Spec 26/14c-I-4 — synthesise the TAH initial-
                        // state dump per docs/clean-room-specs/TRIBES-
                        // INITIAL-BURST.md. The orchestrator produces
                        // per-session-correct VC headers (so retransmits
                        // and acks work) and the scope-always-complete
                        // bit on the last packet, which the previous
                        // canned-byte replay (kTahFirstGhostBurst[]) did
                        // not — TAH stayed stuck in "loading" because
                        // the bit was tied to the captured session's
                        // state and never satisfied the new client.
                        TahBurstOrchestrator orch;
                        auto burst = orch.build_initial_burst(
                            *sess, /*mission*/ nullptr, now_ms);
                        for (auto& p : burst) {
                            if (impl_->socket.send_to(peer, p.data(), p.size())) {
                                total_sent += p.size();
                                ++pkt_count;
                            }
                        }
                    } else {
                        if (impl_->socket.send_to(peer, kFirstGhostBurstTemplate,
                                                  sizeof(kFirstGhostBurstTemplate))) {
                            total_sent = sizeof(kFirstGhostBurstTemplate);
                            pkt_count = 1;
                        }
                    }
                    if (pkt_count > 0) sess->last_outbound_ms = now_ms;
                    std::lock_guard<std::mutex> lk(impl_->mu);
                    ++impl_->stats.data_packets_received;
                    if (pkt_count > 0) {
                        impl_->stats.ghost_bursts_sent += pkt_count;
                        std::fprintf(stderr,
                            "[listener] DataPacket from %s:%u (slot %u, %s), replied ghost burst (%zu pkts / %zuB)\n",
                            peer.host.c_str(), peer.port, sess->player_slot,
                            sess->is_tah_session ? "TAH" : "vanilla",
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
                    bool should_send = cfg_.enable_canned_burst
                        && !sess->ghost_burst_sent;
                    if (should_send) {
                        sess->ghost_burst_sent = true;
                        std::size_t total_sent = 0;
                        std::size_t pkt_count = 0;
                        if (sess->is_tah_session) {
                            // Spec 26/14c-I-4 — see comment on the other
                            // TAH-burst-emit site above. Same orchestrator
                            // path used here for the non-movecmd-fallback
                            // case (TAH's progression events arrive via
                            // arbitrarily-shaped DataPackets, not the
                            // canonical pure-ack the first-data path
                            // expects).
                            TahBurstOrchestrator orch;
                            auto burst = orch.build_initial_burst(
                                *sess, /*mission*/ nullptr, now_ms);
                            for (auto& p : burst) {
                                if (impl_->socket.send_to(peer, p.data(), p.size())) {
                                    total_sent += p.size();
                                    ++pkt_count;
                                }
                            }
                        } else {
                            if (impl_->socket.send_to(peer, kFirstGhostBurstTemplate,
                                                      sizeof(kFirstGhostBurstTemplate))) {
                                total_sent = sizeof(kFirstGhostBurstTemplate);
                                pkt_count = 1;
                            }
                        }
                        if (pkt_count > 0) sess->last_outbound_ms = now_ms;
                        std::lock_guard<std::mutex> lk(impl_->mu);
                        ++impl_->stats.data_packets_received;
                        if (pkt_count > 0) {
                            impl_->stats.ghost_bursts_sent += pkt_count;
                            std::fprintf(stderr,
                                "[listener] non-movecmd DataPacket %zuB from %s:%u (slot %u, %s), replied ghost burst (%zu pkts / %zuB)\n",
                                buf.size(), peer.host.c_str(), peer.port,
                                sess->player_slot,
                                sess->is_tah_session ? "TAH" : "vanilla",
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
            // Drop emitters whose session no longer exists.
            for (auto it = impl_->emitters.begin(); it != impl_->emitters.end(); ) {
                bool alive = false;
                for (auto* s : active) {
                    if (s && s->peer.host == it->first.host
                        && s->peer.port == it->first.port) {
                        alive = true; break;
                    }
                }
                if (!alive) it = impl_->emitters.erase(it);
                else ++it;
            }
            // Build the snapshot once; share by const-ref with every emitter.
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
            for (auto* s : active) {
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

int server_listener_selftest()
{
    using studio::content::net::Endpoint;
    using studio::content::net::UdpSocket;

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
