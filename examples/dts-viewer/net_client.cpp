// Spec 20/15 — net-client integration for dts-viewer.
//
// Background-thread driver that publishes a `net20::GhostRegistry` snapshot
// to the render thread. Two entry points:
//   * `start_replay(path)` — read a capture JSON, feed every s->c packet
//     through `parse_typed_packet`, apply each batch to the registry,
//     and sleep for the per-packet `t_ms` delta so the playback looks
//     like the original session timing.
//   * `start_live(host, port, use_groove)` — drive the template-paste
//     handshake against a live Tribes server. Same code path as
//     net-test-client's `--template-paste --decode-ghosts --ack
//     --send-ready` mode (spec 20/22), simplified to publish into the
//     shared registry instead of dumping packets to JSON.

#include "net_client.hpp"

#include <osengine/client_events.hpp>
#include <osengine/ghost_stream.hpp>
#include <osengine/movecommand.hpp>
#include <osengine/reliable_acks.hpp>

#include "content/net/udp_socket.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace dts_viewer {

namespace {

std::uint64_t now_ms()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// 27B vanilla and 45B Groove RequestConnect templates — copied verbatim
// from `net-test-client/main.cpp`. Keeping a local copy avoids exposing
// internal linkage from that translation unit; if either template ever
// needs to change, update both call sites.
const std::uint8_t kRealRequestConnectTemplate[27] = {
    0x07, 0x00, 0x13, 0x44, 0xa7, 0xe5, 0x18,
    0xad, 0xa7, 0x81,
    0x12, 0x01, 0x00, 0x0d, 0x56, 0xc6, 0xbb, 0x6f, 0x07, 0xc4, 0x52,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

const std::uint8_t kGrooveRequestConnectTemplate[45] = {
    0x05, 0x00, 0x12, 0x41, 0x81, 0xa1, 0xb1, 0xc7, 0xfa, 0x18,
    0x7a, 0x90, 0x0a, 0x15, 0x01, 0x00, 0x0d, 0x56, 0xc6, 0xbb,
    0x6f, 0x09, 0xc4, 0x32, 0x1a, 0x11, 0xc0, 0x03, 0x89, 0xf1,
    0x13, 0x7c, 0x02, 0x6f, 0xa7, 0x7c, 0x7c, 0x01, 0xca, 0xf1,
    0x62, 0x82, 0x5d, 0xb6, 0x07,
};

const std::uint8_t kFirstDataPacket[4] = { 0x07, 0x08, 0x09, 0x80 };

// Decode a JSON hex field — the same ad-hoc scanner used in
// net-test-client/main.cpp::run_replay, copied locally so dts-viewer
// doesn't have to pull in nlohmann_json just for this.
bool extract_hex_packets(const std::string& text,
                         std::vector<std::pair<std::uint64_t,
                                               std::vector<std::uint8_t>>>& out)
{
    auto hexnib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    std::size_t pos = 0;
    while (true) {
        const std::size_t hex_key = text.find("\"hex\"", pos);
        if (hex_key == std::string::npos) break;
        const std::size_t colon = text.find(':', hex_key);
        if (colon == std::string::npos) break;
        const std::size_t q1 = text.find('"', colon);
        if (q1 == std::string::npos) break;
        const std::size_t q2 = text.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        const std::string hex = text.substr(q1 + 1, q2 - q1 - 1);

        // Pull the enclosing object's "dir" and "t_ms" fields if present.
        std::string dir;
        std::uint64_t t_ms = 0;
        const std::size_t obj_start = text.rfind('{', hex_key);
        if (obj_start != std::string::npos) {
            const std::size_t dir_key = text.find("\"dir\"", obj_start);
            if (dir_key != std::string::npos && dir_key < hex_key) {
                const std::size_t dq1 = text.find('"', text.find(':', dir_key));
                const std::size_t dq2 = text.find('"', dq1 + 1);
                if (dq1 != std::string::npos && dq2 != std::string::npos)
                    dir = text.substr(dq1 + 1, dq2 - dq1 - 1);
            }
            const std::size_t tkey = text.find("\"t_ms\"", obj_start);
            if (tkey != std::string::npos && tkey < hex_key) {
                const std::size_t tcol = text.find(':', tkey);
                if (tcol != std::string::npos) {
                    std::size_t i = tcol + 1;
                    while (i < text.size() &&
                           (text[i] == ' ' || text[i] == '\t')) ++i;
                    std::uint64_t v = 0;
                    while (i < text.size() && text[i] >= '0'
                           && text[i] <= '9') {
                        v = v * 10 + (text[i] - '0');
                        ++i;
                    }
                    t_ms = v;
                }
            }
        }
        pos = q2 + 1;
        if (!dir.empty() && dir != "s->c") continue;

        std::vector<std::uint8_t> bytes;
        bytes.reserve(hex.size() / 2);
        for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
            const int hi = hexnib(hex[i]);
            const int lo = hexnib(hex[i + 1]);
            if (hi < 0 || lo < 0) { bytes.clear(); break; }
            bytes.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
        }
        if (!bytes.empty()) out.emplace_back(t_ms, std::move(bytes));
    }
    return true;
}

}  // namespace

NetClient::NetClient() = default;

NetClient::~NetClient() { stop(); }

void NetClient::stop()
{
    if (io_thread_.joinable()) {
        stop_requested_.store(true, std::memory_order_release);
        io_thread_.join();
    }
    running_.store(false, std::memory_order_release);
}

net20::GhostRegistry NetClient::snapshot_registry() const
{
    std::lock_guard<std::mutex> g(mu_);
    return registry_;
}

std::string NetClient::last_error() const
{
    std::lock_guard<std::mutex> g(mu_);
    return last_error_;
}

void NetClient::apply_registry(const net20::GhostRegistry& new_reg)
{
    std::lock_guard<std::mutex> g(mu_);
    registry_ = new_reg;
}

void NetClient::set_last_error(const std::string& s)
{
    std::lock_guard<std::mutex> g(mu_);
    last_error_ = s;
}

bool NetClient::start_replay(const std::string& path)
{
    {
        std::ifstream probe(path);
        if (!probe) {
            set_last_error("open '" + path + "' failed");
            return false;
        }
    }
    stop();
    stop_requested_.store(false);
    running_.store(true);
    io_thread_ = std::thread(&NetClient::replay_thread_main, this, path);
    return true;
}

bool NetClient::start_live(const std::string& host, std::uint16_t port,
                           bool use_groove)
{
    stop();
    stop_requested_.store(false);
    running_.store(true);
    io_thread_ = std::thread(&NetClient::live_thread_main, this,
                             host, port, use_groove);
    return true;
}

void NetClient::replay_thread_main(std::string path)
{
    std::ifstream f(path);
    std::ostringstream ss; ss << f.rdbuf();
    const std::string text = ss.str();

    std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>> packets;
    extract_hex_packets(text, packets);
    std::fprintf(stderr, "[net-client/replay] %zu s->c packets from %s\n",
        packets.size(), path.c_str());

    net20::GhostRegistry reg;
    reg.install_default_class_tag_map();

    // The capture's t_ms are wall-clock-ms-since-capture-start. Replay
    // them at real time so the demo looks like the original session. If
    // the file's worth of packets fits inside ~2s we still cap a small
    // sleep so the registry doesn't fill all-at-once at frame 0 (would
    // hide any animation in the data).
    const std::uint64_t real_start = now_ms();
    std::uint64_t origin_t = packets.empty() ? 0 : packets.front().first;

    for (std::size_t i = 0; i < packets.size(); ++i) {
        if (stop_requested_.load(std::memory_order_acquire)) break;
        const auto& [t_ms, bytes] = packets[i];

        // Pace the playback. We deliberately compress very long
        // capture gaps to 250 ms so e.g. a 15s pause between bursts
        // doesn't freeze the demo, but otherwise honour the original
        // cadence.
        const std::uint64_t want_offset = (t_ms > origin_t) ? (t_ms - origin_t) : 0;
        const std::uint64_t now_off = now_ms() - real_start;
        if (want_offset > now_off) {
            std::uint64_t to_sleep = want_offset - now_off;
            if (to_sleep > 250) to_sleep = 250;
            std::this_thread::sleep_for(std::chrono::milliseconds(to_sleep));
        }

        auto td = net20::parse_typed_packet(bytes.data(), bytes.size(), reg);
        (void)td;
        packets_seen_.fetch_add(1, std::memory_order_relaxed);
        typed_records_.fetch_add(static_cast<int>(td.records.size()),
                                 std::memory_order_relaxed);
        apply_registry(reg);
    }

    std::fprintf(stderr,
        "[net-client/replay] done — %d packets, %d typed records, "
        "%zu statics, %zu players, %zu projectiles, %zu items, %zu vehicles\n",
        packets_seen_.load(), typed_records_.load(),
        reg.statics.size(), reg.players.size(), reg.projectiles.size(),
        reg.items.size(), reg.vehicles.size());

    running_.store(false, std::memory_order_release);
}

void NetClient::live_thread_main(std::string host, std::uint16_t port,
                                 bool use_groove)
{
    using namespace studio::content::net;

    UdpSocket sock;
    if (!sock.bind(0)) {
        set_last_error("bind failed: " + sock.last_error());
        running_.store(false, std::memory_order_release);
        return;
    }

    std::random_device rd;
    std::vector<std::uint8_t> pkt_vec;
    if (use_groove) {
        pkt_vec.assign(std::begin(kGrooveRequestConnectTemplate),
                       std::end(kGrooveRequestConnectTemplate));
    } else {
        pkt_vec.assign(std::begin(kRealRequestConnectTemplate),
                       std::end(kRealRequestConnectTemplate));
        // Spec 20/23 — Groove nonce is at offsets 10..12 (not 7..9
        // like vanilla); bytes 2..9 + 27..44 are auth-protected.
        pkt_vec[10] = static_cast<std::uint8_t>(rd() & 0xff);
        pkt_vec[11] = static_cast<std::uint8_t>(rd() & 0xff);
        pkt_vec[12] = static_cast<std::uint8_t>(rd() & 0xff);
    }
    const auto dst = resolve_endpoint(host, port);
    if (!dst) {
        set_last_error("resolve " + host + " failed");
        running_.store(false, std::memory_order_release);
        return;
    }
    if (!sock.send_to(*dst, pkt_vec.data(), pkt_vec.size())) {
        set_last_error("send RequestConnect failed: " + sock.last_error());
        running_.store(false, std::memory_order_release);
        return;
    }
    std::fprintf(stderr, "[net-client/live] sent RequestConnect (%zuB) to %s:%u\n",
        pkt_vec.size(), host.c_str(), port);

    // Phase 1: AcceptConnect. Spec 20/23 — capture the server-chosen
    // connect-handle parity bit (bit 1 of reply byte 0) so the phase-2
    // ack + downstream pure-acks echo it back. The hardcoded
    // parity=1 from kFirstDataPacket only works in sessions where the
    // server happens to pick parity=1.
    const std::uint64_t accept_deadline = now_ms() + 3000;
    bool got_accept = false;
    bool server_connect_parity = false;
    const std::size_t accept_len = use_groove ? 18 : 16;
    while (now_ms() < accept_deadline
           && !stop_requested_.load(std::memory_order_acquire)) {
        std::vector<std::uint8_t> buf;
        Endpoint src;
        if (sock.try_recv(buf, src)) {
            if (buf.size() == accept_len) {
                server_connect_parity = (buf[0] & 0x02) != 0;
                got_accept = true;
                std::fprintf(stderr,
                    "[net-client/live] phase-1 AcceptConnect ok (%zuB, "
                    "server parity=%d)\n",
                    buf.size(), server_connect_parity ? 1 : 0);
            } else {
                std::string reason((const char*)(buf.data() + 8),
                                   buf.size() - 8);
                while (!reason.empty() && reason.back() == '\0')
                    reason.pop_back();
                set_last_error("RejectConnect — " + reason);
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!got_accept) {
        if (last_error().empty())
            set_last_error("no AcceptConnect within 3s");
        running_.store(false, std::memory_order_release);
        return;
    }

    // Phase 2: first DataPacket — 4-byte pure-ack at send_seq=1 with
    // the server's parity. Spec 20/23.
    std::uint8_t phase2_packet[4];
    {
        net20::BitWriter w;
        w.write_flag(true);
        w.write_flag(server_connect_parity);
        w.write_bits(1u, 9);
        w.write_bits(1u, 5);
        w.write_bits(1u, 3);
        w.write_bits(1u, 5);
        w.write_bits(0u, 3);
        w.write_bits(16u, 5);
        if (w.bytes.size() == 4) {
            std::memcpy(phase2_packet, w.bytes.data(), 4);
        } else {
            set_last_error("phase-2 encoder bug");
            running_.store(false, std::memory_order_release);
            return;
        }
    }
    if (!sock.send_to(*dst, phase2_packet, sizeof(phase2_packet))) {
        set_last_error("phase-2 send failed: " + sock.last_error());
        running_.store(false, std::memory_order_release);
        return;
    }

    const bool connect_parity = server_connect_parity;
    const std::uint16_t our_send_seq = 1;
    const std::uint16_t ready_send_seq = 2;
    bool ready_sent = false;

    net20::AckTracker ack;
    net20::GhostRegistry reg;
    reg.install_default_class_tag_map();

    constexpr std::uint64_t kAckIntervalMs = 35;
    constexpr std::uint64_t kMinEmitMs = 32;
    std::uint64_t last_emit_ms = now_ms();

    auto emit_pure_ack = [&]() {
        const std::uint64_t now = now_ms();
        if (now < last_emit_ms + kMinEmitMs) return;
        auto runs = net20::build_ack_runs(ack.received, ack.highest_recv_mod32);
        if (runs.empty()) return;
        net20::VcHeaderInputs hdr;
        hdr.send_seq = our_send_seq;
        hdr.connect_parity = connect_parity;
        hdr.highest_acked_of_mine =
            static_cast<std::uint8_t>(ack.highest_recv_mod32 & 0x1Fu);
        hdr.ack_runs = std::move(runs);
        hdr.type_word = net20::pkt_type::kPureAck;
        const auto wire = net20::encode_vc_header(hdr);
        sock.send_to(*dst, wire.data(), wire.size());
        last_emit_ms = now;
        ack.total_acks_sent += 1;
        ack.clear_pending();
    };

    auto emit_ready = [&]() {
        net20::ClientReadyInputs in;
        in.send_seq = ready_send_seq;
        in.connect_parity = connect_parity;
        in.highest_acked_of_mine =
            static_cast<std::uint8_t>(ack.highest_recv_mod32 & 0x1Fu);
        in.ack_runs = net20::build_ack_runs(ack.received, ack.highest_recv_mod32);
        in.arg_byte = 'A';
        // Spec 20/23 — use verbatim body bytes (Huffman + input PSC
        // payload undecoded, so we replay the captured working
        // session's 55 body bytes after our own VC header).
        const auto wire = net20::encode_client_ready_verbatim(in);
        if (!sock.send_to(*dst, wire.data(), wire.size())) return;
        ready_sent = true;
        ack.clear_pending();
        last_emit_ms = now_ms();
        std::fprintf(stderr, "[net-client/live] connection-progression event sent\n");
    };

    // Spec 20/19 — send move commands per §17. Once ready has fired,
    // every ~33 ms we emit a 15-byte input frame at the next available
    // send-seq. The frame carries an inline ack of any pending server
    // seqs (so we don't need separate pure-acks while moves flow).
    std::uint16_t move_send_seq = 3;            // first non-ready seq
    std::uint32_t move_seq_counter = 0;          // §17 first-move-seq field
    std::uint64_t last_move_emit_ms = 0;
    int           moves_sent = 0;
    constexpr std::uint64_t kMoveIntervalMs = 33;
    auto emit_move = [&]() {
        net20::MoveCommandInputs in;
        in.send_seq = move_send_seq;
        in.connect_parity = connect_parity;
        in.highest_acked_of_mine =
            static_cast<std::uint8_t>(ack.highest_recv_mod32 & 0x1Fu);
        in.ack_runs = net20::build_ack_runs(ack.received,
                                             ack.highest_recv_mod32);
        in.first_move_seq = move_seq_counter;
        // Single idle move per emit: all axes 0, no flags, no pitch/turn.
        net20::MoveInput m;
        in.moves.push_back(m);
        const auto wire = net20::encode_movecommand(in);
        if (!sock.send_to(*dst, wire.data(), wire.size())) return;
        ++move_send_seq;
        if (move_send_seq > 511) move_send_seq = 0;
        ++move_seq_counter;
        ack.clear_pending();
        last_emit_ms = now_ms();
        last_move_emit_ms = last_emit_ms;
        ++moves_sent;
    };

    while (!stop_requested_.load(std::memory_order_acquire)) {
        std::vector<std::uint8_t> buf;
        Endpoint src;
        if (sock.try_recv(buf, src)) {
            packets_seen_.fetch_add(1, std::memory_order_relaxed);
            net20::ParsedIncomingHeader hdr_in;
            const bool parsed_hdr = net20::parse_incoming_header(
                buf.data(), buf.size(), hdr_in);
            if (parsed_hdr && hdr_in.base_type != net20::pkt_type::kPing) {
                ack.on_receive(hdr_in.send_seq);
            }
            auto td = net20::parse_typed_packet(buf.data(), buf.size(), reg);
            (void)td;
            typed_records_.fetch_add(static_cast<int>(td.records.size()),
                                     std::memory_order_relaxed);
            apply_registry(reg);

            if (!ready_sent && ack.pending_count() > 0) emit_ready();
            if (parsed_hdr && ack.should_force_ack()) emit_pure_ack();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        // Spec 20/19: once ready has fired, emit moves at ~30Hz. Moves
        // carry inline acks so we don't need separate pure-acks while
        // moves are flowing.
        if (ready_sent && now_ms() >= last_move_emit_ms + kMoveIntervalMs) {
            emit_move();
        } else if (ack.pending_count() > 0
                   && now_ms() >= last_emit_ms + kAckIntervalMs) {
            emit_pure_ack();
        }
    }

    std::fprintf(stderr,
        "[net-client/live] stopped — %d packets, %d records, %d moves sent\n",
        packets_seen_.load(), typed_records_.load(), moves_sent);

    running_.store(false, std::memory_order_release);
}

}  // namespace dts_viewer
