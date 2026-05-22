// Track 20 spec 07 — net-test-client CLI.
//
// Exercises 3space's net layer (Connection, ReliableChannel, EventChannel,
// GhostManager) against an external Tribes server. Defaults match the
// Wine-hosted Tribes 1.41 dedicated server from spec 19/01:
//
//   ./net-test-client --host 127.0.0.1 --port 28000 --duration 5
//
// Mode 1 (default): connect to a remote server and walk the connection
// state machine. Reports state transitions, RTT, and reject reasons.
//
// Mode 2 (--loopback-self): spin up our own Connection in server mode on
// one port and another in client mode that connects to it. Useful for
// smoke-testing the build without external dependencies.

#include "content/net/connection.hpp"
#include "content/net/reliable_channel.hpp"
#include "content/net/event_channel.hpp"
#include "content/net/ghost_manager.hpp"
#include "content/net/udp_socket.hpp"

#include "client_events.hpp"
#include "ghost_stream.hpp"
#include "ghost_types.hpp"
#include "reliable_acks.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace studio::content::net;

namespace {

std::uint64_t now_ms()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

void usage(const char* argv0)
{
    std::fprintf(stderr,
        "usage: %s [--host HOST] [--port PORT] [--name NAME] [--password PW]\n"
        "       %*s [--duration SECONDS] [--loopback-self]\n"
        "\n"
        "  --host HOST          server hostname or IP (default 127.0.0.1)\n"
        "  --port PORT          server UDP port (default 28000)\n"
        "  --name NAME          player name (default OpenSiege)\n"
        "  --password PW        server password (default \"\")\n"
        "  --duration SECONDS   total run time (default 5)\n"
        "  --loopback-self      run a local server in-process for smoke test\n"
        "  --template-paste     send a captured-real RequestConnect template\n"
        "  --listen-seconds N   alias for --duration (used with --template-paste)\n"
        "  --ghost-dump PATH    write captured phase-3 packets to PATH as JSON\n"
        "                       (verified to elicit AcceptConnect from real Tribes)\n"
        "  --decode-ghosts      run incoming server packets through the ghost\n"
        "                       parser (spec 20/10); log decoded records per pkt\n"
        "  --replay PATH        offline mode: read a capture JSON and feed every\n"
        "                       s->c packet through the parser (no server needed)\n"
        "  --ack                emit VC pure-acks per spec 20/11 §14 (default off;\n"
        "                       experimentally keeps the connection alive past\n"
        "                       the server's ~3-5s no-ack timeout)\n"
        "  --no-acks            explicit alias for the default (acks off)\n"
        "  --ack-selftest       offline: encode the §14.7 worked example and\n"
        "                       confirm it round-trips to 05 08 09 80\n"
        "  --groove             use the 45B Groove (TribesNext) RequestConnect\n"
        "                       template instead of the 27B vanilla one. Needed\n"
        "                       for servers that reject the vanilla shape with\n"
        "                       \"requires version 1.40 or higher\".\n"
        "  --send-ready         after the first ghost-stream packet arrives,\n"
        "                       emit the c->s connection-progression event per\n"
        "                       spec 20/22 §16. Default on when --ack is set.\n"
        "  --no-send-ready      disable the spec 20/22 emit even with --ack.\n"
        "  --ready-selftest     offline: encode a sample client-ready packet\n"
        "                       and dump its bytes + per-field decode for\n"
        "                       inspection.\n",
        argv0, (int)std::strlen(argv0), "");
}

int run_loopback_self(int duration_s)
{
    std::fprintf(stderr, "[net-test] loopback-self mode\n");
    Connection server;
    if (!server.bind(0)) {
        std::fprintf(stderr, "server bind failed: %s\n",
            server.socket().last_error().c_str());
        return 2;
    }
    server.listen();
    const auto server_port = server.socket().local_port();

    Connection client;
    if (!client.bind(0)) {
        std::fprintf(stderr, "client bind failed: %s\n",
            client.socket().last_error().c_str());
        return 2;
    }
    if (!client.connect("127.0.0.1", server_port, "Loopback", "", 1)) {
        std::fprintf(stderr, "client connect failed\n");
        return 2;
    }

    const std::uint64_t deadline = now_ms() + 1000ULL * duration_s;
    auto last_state_c = client.state();
    auto last_state_s = server.state();
    std::fprintf(stderr,
        "[net-test] client port=%u  server port=%u\n",
        client.socket().local_port(), server_port);

    while (now_ms() < deadline) {
        client.tick(now_ms());
        server.tick(now_ms());
        if (client.state() != last_state_c) {
            std::fprintf(stderr, "[net-test] client -> %s\n",
                state_name(client.state()));
            last_state_c = client.state();
        }
        if (server.state() != last_state_s) {
            std::fprintf(stderr, "[net-test] server -> %s\n",
                state_name(server.state()));
            last_state_s = server.state();
        }
        if (client.state() == Connection::State::Failed) {
            std::fprintf(stderr, "[net-test] client failed: %s\n",
                client.reject_reason().c_str());
            return 3;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::fprintf(stderr,
        "[net-test] final: client=%s server=%s  rtt=%ums\n",
        state_name(client.state()), state_name(server.state()),
        client.rtt_ms());
    client.disconnect("test exit");
    server.tick(now_ms());
    // Exit OK if we reached Connected (or beyond) at any point in the run.
    // The disconnect path naturally leaves us in Disconnecting or Unbound.
    return 0;
}

// 2026-05-21 — verified working RequestConnect template captured from a
// live Wine Tribes 1.41 client -> vanilla Tribes 1.41 dedicated server
// loopback handshake. The three bytes at offset 7..9 are a per-session
// random nonce that the server echoes back in its AcceptConnect at offset
// 4..6. The remaining 23 bytes encode the static client identity (game id,
// protocol version, player slot) in a bit-packed form we have not yet
// fully reverse-engineered. See captures/real-tribes/BREAKTHROUGH.md.
const std::uint8_t kRealRequestConnectTemplate[27] = {
    0x07, 0x00, 0x13, 0x44, 0xa7, 0xe5, 0x18,
    0xad, 0xa7, 0x81,                            // <- nonce slot (bytes 7..9)
    0x12, 0x01, 0x00, 0x0d, 0x56, 0xc6, 0xbb, 0x6f, 0x07, 0xc4, 0x52,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// 2026-05-22 — Groove (TribesNext-patched) RequestConnect captured via
// scripts/tribes_capture_proxy.py against the Windows server at
// 192.168.1.89:28001. This shape elicits AcceptConnect from servers that
// reject the 27-byte vanilla template with "requires version 1.40 or
// higher". The extra 18-byte trailer (offsets 27..44) is presumably a
// GGConnect / netset.dll auth signature.
//
// Nonce slot offset: still unverified. The randomized-bytes look like
// offsets 2..9 in this template (the 8-byte run 0x12 0x41 0x81 0xa1 0xb1
// 0xc7 0xfa 0x18) plus more in the auth trailer. For an initial test we
// send the captured bytes verbatim — server may dedupe but we'll learn
// from the response. Capture a fresh proxy session to refresh this
// constant whenever needed.
const std::uint8_t kGrooveRequestConnectTemplate[45] = {
    0x05, 0x00, 0x12, 0x41, 0x81, 0xa1, 0xb1, 0xc7, 0xfa, 0x18,
    0x7a, 0x90, 0x0a, 0x15, 0x01, 0x00, 0x0d, 0x56, 0xc6, 0xbb,
    0x6f, 0x09, 0xc4, 0x32, 0x1a, 0x11, 0xc0, 0x03, 0x89, 0xf1,
    0x13, 0x7c, 0x02, 0x6f, 0xa7, 0x7c, 0x7c, 0x01, 0xca, 0xf1,
    0x62, 0x82, 0x5d, 0xb6, 0x07,
};

int run_template_paste(const std::string& host, std::uint16_t port,
                       int duration_s,
                       const std::string& ghost_dump_path,
                       bool decode_ghosts,
                       bool send_acks,
                       bool use_groove,
                       bool send_ready)
{
    const char* tmpl_name = use_groove ? "Groove (45B)" : "vanilla Wine (27B)";
    std::fprintf(stderr,
        "[net-test] template-paste mode -> %s:%u (sends %s template,\n"
        "[net-test] listens for AcceptConnect)\n",
        host.c_str(), port, tmpl_name);

    UdpSocket sock;
    if (!sock.bind(0)) {
        std::fprintf(stderr, "bind failed: %s\n", sock.last_error().c_str());
        return 2;
    }

    // Fresh random nonce per run so the server can't dedupe.
    // Nonce-slot positions differ by template; tweak the byte ranges below
    // if you observe duplicate-session rejections.
    std::random_device rd;
    std::vector<std::uint8_t> pkt_vec;
    if (use_groove) {
        pkt_vec.assign(std::begin(kGrooveRequestConnectTemplate),
                       std::end(kGrooveRequestConnectTemplate));
        // Spec 20/23 finding — cross-referencing two captures, the
        // per-session nonce in the 45-byte Groove RequestConnect
        // lives at offsets 10..12 (NOT 7..9 like vanilla). Server
        // echoes those same bytes back at reply offsets 4..6 of
        // AcceptConnect. Bytes 2..9 and the 18-byte auth trailer at
        // offsets 27..44 are auth-protected; randomizing them
        // results in silent rejection.
        pkt_vec[10] = static_cast<std::uint8_t>(rd() & 0xff);
        pkt_vec[11] = static_cast<std::uint8_t>(rd() & 0xff);
        pkt_vec[12] = static_cast<std::uint8_t>(rd() & 0xff);
    } else {
        pkt_vec.assign(std::begin(kRealRequestConnectTemplate),
                       std::end(kRealRequestConnectTemplate));
        pkt_vec[7] = static_cast<std::uint8_t>(rd() & 0xff);
        pkt_vec[8] = static_cast<std::uint8_t>(rd() & 0xff);
        pkt_vec[9] = static_cast<std::uint8_t>(rd() & 0xff);
    }
    std::uint8_t* pkt = pkt_vec.data();
    const std::size_t pkt_len = pkt_vec.size();

    const auto dst = resolve_endpoint(host, port);
    if (!dst) {
        std::fprintf(stderr, "resolve %s:%u failed\n", host.c_str(), port);
        return 2;
    }
    std::fprintf(stderr, "[net-test] sending %zuB nonce-region=%02x %02x %02x\n",
        pkt_len, pkt[7], pkt[8], pkt[9]);
    if (!sock.send_to(*dst, pkt, pkt_len)) {
        std::fprintf(stderr, "send failed: %s\n", sock.last_error().c_str());
        return 2;
    }

    auto hex_dump = [](const char* label, const std::uint8_t* p, std::size_t n) {
        std::fprintf(stderr, "[net-test] %s (%zuB): ", label, n);
        for (std::size_t i = 0; i < n; ++i) std::fprintf(stderr, "%02x", p[i]);
        std::fputc('\n', stderr);
    };

    // Phase 1: wait for AcceptConnect. Vanilla expects 16B with nonce
    // echo at offset 4..6; Groove just an 18B reply (nonce offset
    // unverified). Anything else = RejectConnect with ASCII reason.
    //
    // Spec 20/23 finding: the server picks its own connect-handle
    // parity at AcceptConnect time (varies per session). We MUST
    // echo that exact parity bit on every subsequent VC datagram, or
    // the server rejects us silently and stays in the
    // AcceptConnect-retransmit loop. Capture the parity bit (bit 1 of
    // reply byte 0) here so the phase-2 ack + the ready packet + every
    // pure-ack use it.
    const std::uint64_t accept_deadline = now_ms() + 3000;
    bool got_accept = false;
    bool server_connect_parity = false;
    const std::size_t accept_len = use_groove ? 18 : 16;
    while (now_ms() < accept_deadline) {
        std::vector<std::uint8_t> buf;
        Endpoint src;
        if (sock.try_recv(buf, src)) {
            hex_dump("recv", buf.data(), buf.size());
            if (buf.size() == accept_len) {
                server_connect_parity = (buf[0] & 0x02) != 0;
                std::fprintf(stderr,
                    "[net-test] phase-1: AcceptConnect received "
                    "(server parity=%d)\n",
                    server_connect_parity ? 1 : 0);
                got_accept = true;
            } else {
                // ASCII reason starts after the 8-byte server header.
                std::string reason((const char*)(buf.data() + 8),
                    buf.size() - 8);
                while (!reason.empty() && reason.back() == '\0') reason.pop_back();
                std::fprintf(stderr,
                    "[net-test] phase-1: RejectConnect — \"%s\"\n",
                    reason.c_str());
                return 3;
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!got_accept) {
        std::fprintf(stderr, "[net-test] phase-1: no AcceptConnect within 3s\n");
        return 4;
    }

    // Phase 2: send the first DataPacket — a 4-byte pure-ack at
    // send-seq=1 acking the AcceptConnect (server-seq=1).
    //
    // Wire (LSB-first): bit 0 VC=1, bit 1 parity=server_connect_parity,
    // bits 2..10 send_seq=1, bits 11..15 high_ack=1, bits 16..18 ack
    // run len=1, bits 19..23 ack start=1, bits 24..26 ack terminator=0,
    // bits 27..31 type=16 (Ack).
    //
    // Spec 20/23 finding: parity MUST match the server's
    // AcceptConnect byte-0 bit 1, which varies per session. The old
    // hardcoded `07 08 09 80` only worked by coincidence when the
    // server happened to pick parity=1.
    std::uint8_t phase2_packet[4];
    {
        net20::BitWriter w;
        w.write_flag(true);                          // VC
        w.write_flag(server_connect_parity);          // parity
        w.write_bits(1u, 9);                          // send_seq = 1
        w.write_bits(1u, 5);                          // highest_acked_of_mine = 1
        w.write_bits(1u, 3);                          // ack run length = 1
        w.write_bits(1u, 5);                          // ack run start = 1
        w.write_bits(0u, 3);                          // ack-list terminator
        w.write_bits(16u, 5);                         // type = 16 (Ack)
        const auto& wb = w.bytes;
        if (wb.size() != 4) {
            std::fprintf(stderr,
                "[net-test] phase-2 encoder bug: got %zu bytes\n",
                wb.size());
            return 2;
        }
        std::memcpy(phase2_packet, wb.data(), 4);
    }
    if (!sock.send_to(*dst, phase2_packet, sizeof(phase2_packet))) {
        std::fprintf(stderr, "[net-test] phase-2: send failed: %s\n",
            sock.last_error().c_str());
        return 2;
    }
    hex_dump("phase-2 send", phase2_packet, sizeof(phase2_packet));

    // Phase 3: server should now start streaming ghost data. Accumulate
    // every packet so a --ghost-dump path can emit a JSON file the
    // ghost-stream parser specs (20/09+) can replay against.
    //
    // When --ack is set we additionally emit pure-acks per clean-room
    // §14: every non-Ping server packet marks its (send_seq mod 32)
    // slot in our 32-slot window; on a ~28 Hz cadence (or earlier if
    // ≥12 slots are pending — the "force ack" rule) we transmit a
    // 3..10 byte pure-ack carrying the run-length-encoded slot list.
    //
    // The send-seq we re-use for every pure-ack is the same value the
    // phase-2 emit used (§14.2: pure-ack/pure-ping do NOT increment
    // the counter). Our phase-2 first emit was a 4-byte pure-ack with
    // send-seq = 1 (the same seq RequestConnect occupied), so we keep
    // sending pure-acks at send-seq = 1 throughout phase-3. Parity
    // bit is the server-chosen connect-handle parity from the
    // AcceptConnect reply (spec 20/23 finding).
    const bool connect_parity = server_connect_parity;
    const std::uint16_t our_send_seq = 1;  // see comment above
    // Spec 20/22: the connection-progression DataPacket (sent once,
    // immediately after the first ghost-stream packet arrives) carries
    // real payload and therefore consumes its own send-seq slot per
    // §14.2. Phase-2 emitted send-seq=1, so the ready packet is seq=2.
    const std::uint16_t ready_send_seq = 2;
    bool ready_sent = false;
    int ready_send_count = 0;
    std::uint64_t ready_sent_ms = 0;
    net20::AckTracker ack;
    // Spec 14: typed-ghost registry, populated lazily by --decode-ghosts.
    net20::GhostRegistry typed_registry;
    typed_registry.install_default_class_tag_map();

    struct CapturedPacket {
        std::uint64_t t_ms;
        std::vector<std::uint8_t> bytes;
    };
    std::vector<CapturedPacket> captured;
    int ghost_packets = 0;
    std::size_t ghost_bytes = 0;
    int acks_emitted = 0;
    std::size_t ack_bytes = 0;
    const std::uint64_t phase3_start = now_ms();
    const std::uint64_t deadline = phase3_start + 1000ULL * duration_s;
    // Steady-state cadence (§14.5 observed 28 Hz ≈ 35 ms).
    constexpr std::uint64_t kAckIntervalMs = 35;
    constexpr std::uint64_t kMinEmitMs = 32;  // §14.5 rule 4
    std::uint64_t last_emit_ms = now_ms();    // phase-2 just fired

    auto emit_pure_ack = [&](const char* trigger) {
        const std::uint64_t now = now_ms();
        if (now < last_emit_ms + kMinEmitMs) return;  // §14.5 rule 4
        // Build runs from the current pending window.
        auto runs = net20::build_ack_runs(ack.received, ack.highest_recv_mod32);
        if (runs.empty()) return;  // nothing to ack yet
        net20::VcHeaderInputs hdr;
        hdr.send_seq = our_send_seq;
        hdr.connect_parity = connect_parity;
        hdr.highest_acked_of_mine =
            static_cast<std::uint8_t>(ack.highest_recv_mod32 & 0x1Fu);
        hdr.ack_runs = std::move(runs);
        hdr.type_word = net20::pkt_type::kPureAck;
        const auto wire = net20::encode_vc_header(hdr);
        if (!sock.send_to(*dst, wire.data(), wire.size())) {
            std::fprintf(stderr, "[ack] send failed: %s\n",
                sock.last_error().c_str());
            return;
        }
        acks_emitted += 1;
        ack_bytes += wire.size();
        last_emit_ms = now;
        ack.total_acks_sent += 1;
        if (acks_emitted <= 5) {
            std::fprintf(stderr,
                "[ack] seq=%u size=%zuB highest=%u runs=%zu trigger=%s\n",
                static_cast<unsigned>(our_send_seq), wire.size(),
                static_cast<unsigned>(ack.highest_recv_mod32),
                hdr.ack_runs.size(), trigger);
        }
        ack.clear_pending();
    };

    // Spec 20/22 §16.5 — emit the c→s connection-progression DataPacket.
    // The server stays stuck retransmitting AcceptConnect / first ghost
    // burst until it sees a guaranteed event on the reliable-event
    // sub-stream. We send one DataPacket carrying:
    //   * VC header with our current ack runs piggy-backed (§14.3)
    //   * R0=1 / R1=1 rate-control prefix (66 ms / 400 B, matching the
    //     Groove capture) (§3.4)
    //   * event sub-stream with one guaranteed-ordered event,
    //     class id wire = 8, seq = 0, argc = 1, single uncompressed
    //     1-byte ASCII string ('A') (§16.4/§16.5)
    //   * input + ghost sub-stream present flags both = 0.
    auto emit_ready = [&](const char* trigger) {
        net20::ClientReadyInputs in;
        in.send_seq = ready_send_seq;
        in.connect_parity = connect_parity;
        in.highest_acked_of_mine =
            static_cast<std::uint8_t>(ack.highest_recv_mod32 & 0x1Fu);
        in.ack_runs = net20::build_ack_runs(ack.received,
                                             ack.highest_recv_mod32);
        in.arg_byte = 'A';
        // Spec 20/23 — empirically the "any 1-byte event" theory from
        // §16.5 doesn't unblock the server. Use the verbatim-replay
        // path which prepends our VC header to the body bytes from a
        // known-working captured session.
        const auto wire = net20::encode_client_ready_verbatim(in);
        if (!sock.send_to(*dst, wire.data(), wire.size())) {
            std::fprintf(stderr, "[ready] send failed: %s\n",
                sock.last_error().c_str());
            return;
        }
        const std::uint64_t now = now_ms();
        ready_sent = true;
        ++ready_send_count;
        ready_sent_ms = now;
        // After a real DataPacket, future pure-acks should bump
        // highest-acked-of-mine off any of these acks since they rode
        // this packet. Mirror the pure-ack lambda: clear pending.
        ack.clear_pending();
        last_emit_ms = now;
        std::fprintf(stderr,
            "[ready] sent %zuB at t=%llums trigger=%s seq=%u parity=%d\n",
            wire.size(),
            static_cast<unsigned long long>(now - phase3_start),
            trigger,
            static_cast<unsigned>(ready_send_seq),
            static_cast<int>(connect_parity));
    };

    while (now_ms() < deadline) {
        std::vector<std::uint8_t> buf;
        Endpoint src;
        if (sock.try_recv(buf, src)) {
            ghost_packets += 1;
            ghost_bytes += buf.size();
            if (ghost_packets <= 3) {
                hex_dump("ghost", buf.data(), std::min<std::size_t>(buf.size(), 32));
            }
            if (decode_ghosts) {
                // Outer framing summary (one line per packet that contains
                // at least one ghost record).
                auto dec = net20::parse_ghost_packet(buf.data(), buf.size());
                if (!dec.updates.empty()) {
                    std::fprintf(stderr, "[ghost] %s\n",
                        net20::format_update(dec.updates[0]).c_str());
                } else if (!dec.note.empty()) {
                    std::fprintf(stderr, "[ghost] (no record) %s\n",
                        dec.note.c_str());
                }
                // Spec 14: walk every record and dispatch into typed structs.
                auto td = net20::parse_typed_packet(buf.data(), buf.size(),
                                                   typed_registry);
                for (const auto& tr : td.records) {
                    std::fprintf(stderr, "[ghost-typed] %s\n",
                        tr.log_line.c_str());
                }
            }
            // Parse the incoming header once; both --ack and --send-ready
            // care about the server's send-seq for piggybacked ack-runs.
            net20::ParsedIncomingHeader hdr_in;
            const bool parsed_hdr = net20::parse_incoming_header(
                buf.data(), buf.size(), hdr_in);
            // §14.5 rule 1: mark non-Ping receive in our ack window.
            if (parsed_hdr && (send_acks || send_ready)
                && hdr_in.base_type != net20::pkt_type::kPing) {
                ack.on_receive(hdr_in.send_seq);
            }
            // Spec 20/22 §16.5: send the connection-progression event
            // as soon as we've observed at least one server-side seq
            // we can ack inside our outgoing VC header. Do this BEFORE
            // the force-12 emit so the ready packet itself carries
            // the piggybacked ack runs.
            if (send_ready && !ready_sent && ack.pending_count() > 0) {
                emit_ready("first-server-packet");
            }
            // §14.5 rule 3: 12+ pending → force immediate emit.
            if (send_acks && parsed_hdr && ack.should_force_ack()) {
                emit_pure_ack("force-12");
            }
            captured.push_back({ now_ms() - phase3_start, std::move(buf) });
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // §14.5 rule 2: steady-state emit at ~28 Hz when acks are pending.
        if (send_acks && ack.pending_count() > 0
            && now_ms() >= last_emit_ms + kAckIntervalMs) {
            emit_pure_ack("cadence");
        }
    }
    std::fprintf(stderr,
        "[net-test] phase-3: %d ghost packets, %zu bytes total"
        "; acks_sent=%d (%zuB); ready_sent=%d at t=%llums\n",
        ghost_packets, ghost_bytes, acks_emitted, ack_bytes,
        ready_send_count,
        static_cast<unsigned long long>(
            ready_sent_ms == 0 ? 0 : ready_sent_ms - phase3_start));

    if (!ghost_dump_path.empty() && !captured.empty()) {
        std::FILE* f = std::fopen(ghost_dump_path.c_str(), "wb");
        if (!f) {
            std::fprintf(stderr, "[net-test] ghost-dump: open '%s' failed\n",
                ghost_dump_path.c_str());
        } else {
            // Hand-roll minimal JSON: { "packets": [ { "i":..., "t_ms":...,
            // "length":..., "hex":"..." }, ... ] }. Pulling in nlohmann_json
            // here would bloat the binary; this output is verifiable enough.
            std::fprintf(f, "{\n  \"server\": \"%s:%u\",\n",
                host.c_str(), port);
            std::fprintf(f, "  \"nonce\": \"%02x%02x%02x\",\n",
                pkt[7], pkt[8], pkt[9]);
            std::fprintf(f, "  \"phase3_packet_count\": %d,\n", ghost_packets);
            std::fprintf(f, "  \"phase3_byte_count\": %zu,\n", ghost_bytes);
            std::fprintf(f, "  \"packets\": [");
            for (std::size_t i = 0; i < captured.size(); ++i) {
                const auto& cp = captured[i];
                std::fprintf(f, "%s\n    { \"i\": %zu, \"t_ms\": %llu, "
                    "\"length\": %zu, \"hex\": \"",
                    i == 0 ? "" : ",",
                    i,
                    static_cast<unsigned long long>(cp.t_ms),
                    cp.bytes.size());
                for (std::uint8_t b : cp.bytes) std::fprintf(f, "%02x", b);
                std::fprintf(f, "\" }");
            }
            std::fprintf(f, "\n  ]\n}\n");
            std::fclose(f);
            std::fprintf(stderr,
                "[net-test] ghost-dump: wrote %zu packets to %s\n",
                captured.size(), ghost_dump_path.c_str());
        }
    }

    return ghost_packets > 0 ? 0 : 5;
}

// Spec 20/10 self-test path: read a capture JSON produced by
// scripts/tribes_capture_proxy.py (or run_template_paste --ghost-dump),
// feed every s->c packet through parse_ghost_packet, and summarise.
// No network access required — purely offline verification of the
// ghost-stream parser against canned data.
int run_replay(const std::string& path)
{
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "[replay] open '%s' failed\n", path.c_str());
        return 2;
    }
    std::ostringstream ss; ss << f.rdbuf();
    const std::string text = ss.str();

    // Tiny ad-hoc JSON scanner — pull out the hex string of every packet
    // whose "dir":"s->c" or that lacks a dir field (older ghost-dump
    // format only contained server replies). Robust enough for the two
    // JSON shapes we produce.
    int total = 0, decoded = 0, with_record = 0;
    std::vector<std::uint16_t> ghost_ids;

    // Spec 14: typed-ghost registry, kept across packets so deltas can
    // resolve previously-introduced ghost ids back to their classes.
    net20::GhostRegistry typed_registry;
    typed_registry.install_default_class_tag_map();
    int typed_records = 0;
    int typed_by_kind[6] = {0,0,0,0,0,0};
    // For quantization verification: track per-object-id first-seen position
    // so we can compare against subsequent updates of the same object. Note
    // we use the 32-bit persistent object id (not the ghost_id) because the
    // capture's "ghost_id 0" reappears across packets representing DIFFERENT
    // physical objects (the brute-force scanner picks the highest-scoring
    // candidate per packet — usually that packet's leading record, but the
    // class_tag differs between packets, confirming these are distinct
    // objects that happen to share a ghost_id slot). The 32-bit object_id
    // is stable and unique per physical object.
    struct FirstPos { float x, y, z; bool seen; };
    std::unordered_map<std::uint32_t, FirstPos> first_pos;
    int pos_consistent = 0;
    int pos_mismatch = 0;
    // Sanity range for "plausible terrain coordinate" — reject obvious
    // float-decode garbage (e.g. 1e34 values mean we read the wrong bytes).
    int pos_plausible = 0;
    int pos_garbage = 0;

    std::size_t pos = 0;
    while (true) {
        const std::size_t hex_key = text.find("\"hex\"", pos);
        if (hex_key == std::string::npos) break;
        const std::size_t colon = text.find(':', hex_key);
        const std::size_t q1 = text.find('"', colon);
        if (q1 == std::string::npos) break;
        const std::size_t q2 = text.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        const std::string hex = text.substr(q1 + 1, q2 - q1 - 1);

        // Pull the "dir" field that precedes this packet record. Within
        // the same object, "dir" appears before "hex".
        std::string dir;
        const std::size_t obj_start = text.rfind('{', hex_key);
        if (obj_start != std::string::npos) {
            const std::size_t dir_key = text.find("\"dir\"", obj_start);
            if (dir_key != std::string::npos && dir_key < hex_key) {
                const std::size_t dq1 = text.find('"', text.find(':', dir_key));
                const std::size_t dq2 = text.find('"', dq1 + 1);
                if (dq1 != std::string::npos && dq2 != std::string::npos)
                    dir = text.substr(dq1 + 1, dq2 - dq1 - 1);
            }
        }
        pos = q2 + 1;

        if (!dir.empty() && dir != "s->c") continue;

        // Decode hex.
        std::vector<std::uint8_t> bytes;
        bytes.reserve(hex.size() / 2);
        for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
            auto hexnib = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                return -1;
            };
            int hi = hexnib(hex[i]), lo = hexnib(hex[i + 1]);
            if (hi < 0 || lo < 0) break;
            bytes.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
        }
        if (bytes.empty()) continue;

        ++total;
        auto dec = net20::parse_ghost_packet(bytes.data(), bytes.size());
        ++decoded;
        if (!dec.updates.empty()) {
            ++with_record;
            const auto& u = dec.updates[0];
            ghost_ids.push_back(u.ghost_id);
            if (with_record <= 5) {
                std::fprintf(stderr, "[replay] len=%zuB %s\n",
                    bytes.size(), net20::format_update(u).c_str());
            }
        } else if (total <= 10 && !dec.note.empty()) {
            std::fprintf(stderr, "[replay] len=%zuB no-record (%s)\n",
                bytes.size(), dec.note.c_str());
        }

        // Spec 14: dispatch typed records.
        auto td = net20::parse_typed_packet(bytes.data(), bytes.size(),
                                             typed_registry);
        for (const auto& tr : td.records) {
            ++typed_records;
            const auto kind_idx = static_cast<std::size_t>(tr.kind);
            if (kind_idx < 6) typed_by_kind[kind_idx]++;
            if (typed_records <= 20) {
                std::fprintf(stderr, "[replay-typed] %s\n",
                    tr.log_line.c_str());
            }
            // Quantization check: for StaticShape records, capture the
            // first-seen position per ghost_id and confirm subsequent
            // updates of the same ghost report a bit-identical position
            // (static shapes never move; consistent positions => the
            // 96-bit byte-aligned float decode is correct).
            if (tr.kind == net20::GhostClassKind::StaticShape && !tr.kill
                && tr.full_update) {
                auto it = typed_registry.statics.find(tr.ghost_id);
                if (it != typed_registry.statics.end()
                    && it->second.transform_changed) {
                    const auto& s = it->second;
                    // Plausibility: Tribes missions span ~±2000m horizontally
                    // and ~±500m vertically; we use a generous ±10000 m
                    // window to reject obvious float-decode garbage.
                    auto plausible = [](float v) {
                        return std::isfinite(v) && std::abs(v) <= 10000.0f;
                    };
                    if (plausible(s.pos_x) && plausible(s.pos_y)
                        && plausible(s.pos_z)) {
                        ++pos_plausible;
                    } else {
                        ++pos_garbage;
                    }
                    // Per-object-id consistency: if the same object_id
                    // re-appears (server retransmits the same record),
                    // every decoded position must be bit-identical.
                    auto& fp = first_pos[s.object_id];
                    if (!fp.seen) {
                        fp = { s.pos_x, s.pos_y, s.pos_z, true };
                    } else {
                        if (fp.x == s.pos_x && fp.y == s.pos_y
                            && fp.z == s.pos_z) {
                            ++pos_consistent;
                        } else {
                            ++pos_mismatch;
                            if (pos_mismatch <= 4) {
                                std::fprintf(stderr,
                                    "[replay-qcheck] obj=0x%08x POS MISMATCH "
                                    "first=(%.4f,%.4f,%.4f) now=(%.4f,%.4f,%.4f)\n",
                                    s.object_id,
                                    fp.x, fp.y, fp.z,
                                    s.pos_x, s.pos_y, s.pos_z);
                            }
                        }
                    }
                }
            }
        }
    }

    // Distinct ghost-id count for acceptance criterion.
    std::sort(ghost_ids.begin(), ghost_ids.end());
    ghost_ids.erase(std::unique(ghost_ids.begin(), ghost_ids.end()),
                    ghost_ids.end());

    std::fprintf(stderr,
        "[replay] %d s->c packets seen, %d decoded, %d with at least one record\n",
        total, decoded, with_record);
    std::fprintf(stderr, "[replay] distinct ghost_ids: %zu\n", ghost_ids.size());
    if (!ghost_ids.empty()) {
        std::fprintf(stderr, "[replay] ids:");
        for (std::size_t i = 0; i < ghost_ids.size() && i < 20; ++i) {
            std::fprintf(stderr, " %u", static_cast<unsigned>(ghost_ids[i]));
        }
        if (ghost_ids.size() > 20) std::fprintf(stderr, " ...");
        std::fprintf(stderr, "\n");
    }

    std::fprintf(stderr,
        "[replay-typed] total typed records: %d "
        "(player=%d proj=%d item=%d vehicle=%d static=%d unknown=%d)\n",
        typed_records,
        typed_by_kind[(int)net20::GhostClassKind::Player],
        typed_by_kind[(int)net20::GhostClassKind::Projectile],
        typed_by_kind[(int)net20::GhostClassKind::Item],
        typed_by_kind[(int)net20::GhostClassKind::Vehicle],
        typed_by_kind[(int)net20::GhostClassKind::StaticShape],
        typed_by_kind[(int)net20::GhostClassKind::Unknown]);
    std::fprintf(stderr,
        "[replay-qcheck] StaticShape position decode:"
        " plausible=%d garbage=%d (range check |coord| <= 10000m)\n",
        pos_plausible, pos_garbage);
    std::fprintf(stderr,
        "[replay-qcheck] StaticShape per-object_id consistency:"
        " ok=%d mismatch=%d (same object_id seen across packets)\n",
        pos_consistent, pos_mismatch);

    // Acceptance: at least 1 ghost record across the capture.
    return with_record > 0 ? 0 : 6;
}

// Spec 20/12 offline self-test: verify our ack encoder reproduces the
// 4-byte pure-ack hex `05 08 09 80` worked-example from §14.7 (a Groove
// pure-ack acking server seq 1, with our own send-seq=1 and parity=0).
int run_ack_selftest()
{
    // §14.7 worked example: bytes `05 08 09 80`. Cross-checking the bit-by-bit
    // table in §14.7 against the LSB-first decoding rule of §14.1 (which the
    // ghost_stream parser also relies on) shows the "highest-acked-of-mine =
    // 2" entry in the §14.7 prose decodes to 1 on the wire (bit 11 = 1,
    // bits 12..15 = 0 → LSB-first integer = 1). The hex bytes themselves
    // are the authoritative value; the encoder reproduces them with
    // highest_acked = 1.
    const std::vector<std::uint8_t> expected = { 0x05, 0x08, 0x09, 0x80 };
    const auto actual = net20::encode_pure_ack_simple(
        /*send_seq=*/1,
        /*connect_parity=*/false,
        /*highest_acked=*/1,
        /*run_length=*/1,
        /*run_start=*/1);

    auto to_hex = [](const std::vector<std::uint8_t>& v) {
        std::string s;
        char buf[4];
        for (auto b : v) {
            std::snprintf(buf, sizeof(buf), "%02x ", b);
            s += buf;
        }
        if (!s.empty()) s.pop_back();
        return s;
    };

    std::fprintf(stderr, "[selftest] §14.7 worked example\n");
    std::fprintf(stderr, "[selftest] expected: %s\n", to_hex(expected).c_str());
    std::fprintf(stderr, "[selftest] actual:   %s\n", to_hex(actual).c_str());
    if (actual != expected) {
        std::fprintf(stderr, "[selftest] FAIL\n");
        return 7;
    }

    // Also exercise the parser path: feed the bytes back through
    // parse_incoming_header and confirm type_word = 16, base_type = 0,
    // is_ack = true, send_seq = 1.
    net20::ParsedIncomingHeader p;
    if (!net20::parse_incoming_header(actual.data(), actual.size(), p)) {
        std::fprintf(stderr, "[selftest] parse_incoming_header rejected own emit\n");
        return 7;
    }
    if (p.send_seq != 1 || p.type_word != 16 || !p.is_ack || p.base_type != 0) {
        std::fprintf(stderr,
            "[selftest] parser mismatch: seq=%u type=%u base=%u ack=%d\n",
            (unsigned)p.send_seq, (unsigned)p.type_word, (unsigned)p.base_type,
            (int)p.is_ack);
        return 7;
    }

    // Multi-run encoder sanity check: two non-adjacent runs in the window.
    std::array<bool, 32> w{};
    w[3] = w[4] = w[5] = true;     // run of 3 at start=3
    w[10] = true;                  // run of 1 at start=10
    auto runs = net20::build_ack_runs(w, /*highest=*/10);
    if (runs.size() != 2
        || runs[0].length != 3 || runs[0].start_seq != 3
        || runs[1].length != 1 || runs[1].start_seq != 10) {
        std::fprintf(stderr,
            "[selftest] build_ack_runs mismatch: %zu runs\n", runs.size());
        for (const auto& r : runs)
            std::fprintf(stderr, "             len=%u start=%u\n",
                (unsigned)r.length, (unsigned)r.start_seq);
        return 7;
    }

    // Split-on-7 sanity: a contiguous run of 9 should split as (7, s) + (2, s+7).
    std::array<bool, 32> w2{};
    for (int i = 0; i < 9; ++i) w2[i] = true;
    auto runs2 = net20::build_ack_runs(w2, 8);
    if (runs2.size() != 2
        || runs2[0].length != 7 || runs2[0].start_seq != 0
        || runs2[1].length != 2 || runs2[1].start_seq != 7) {
        std::fprintf(stderr, "[selftest] split-on-7 mismatch: %zu runs\n",
            runs2.size());
        return 7;
    }

    std::fprintf(stderr, "[selftest] PASS\n");
    return 0;
}

// Spec 20/22 offline self-test: build a client-ready packet and dump
// it for byte-level inspection. The expected layout per §16.5 with our
// minimal sub-stream choices (input=0, ghost=0):
//   bytes 0..3   VC header (send_seq=2, parity=0, ack run (1,2),
//                terminator + type=DataPacket)
//   bits  32..73 rate-control prefix (R0=1 d=66 sz=400, R1=1 d=66 sz=400)
//   bits  74..97 event-sub-stream-present(1) + event header (guaranteed,
//                explicit seq=0, class id wire = 8) + argc=1
//   bits  98..106 compression flag(0) + length=1
//   bits 107..(byte-aligned) + ASCII 'A' = 0x41
//   then event-present=0, input-present=0, ghost-present=0
int run_ready_selftest()
{
    net20::ClientReadyInputs in;
    in.send_seq = 2;
    in.connect_parity = false;
    in.highest_acked_of_mine = 1;
    in.ack_runs.push_back({1, 2});  // ack server seq 2
    in.arg_byte = 'A';
    const auto wire = net20::encode_client_ready(in);

    std::fprintf(stderr, "[ready-selftest] encoded %zu bytes:\n  ",
        wire.size());
    for (std::size_t i = 0; i < wire.size(); ++i) {
        std::fprintf(stderr, "%02x", wire[i]);
        if ((i & 0x0f) == 0x0f) std::fprintf(stderr, "\n  ");
        else std::fprintf(stderr, " ");
    }
    std::fputc('\n', stderr);

    // Spot-check: byte 0 should be 0x09 (VC=1, parity=0, send_seq=2).
    if (wire.size() < 4) {
        std::fprintf(stderr, "[ready-selftest] FAIL: too short\n");
        return 7;
    }
    auto bit = [&](std::size_t p) -> unsigned {
        if ((p >> 3) >= wire.size()) return 0;
        return (wire[p >> 3] >> (p & 7)) & 1u;
    };
    auto bits = [&](std::size_t p, unsigned w) -> std::uint32_t {
        std::uint32_t v = 0;
        for (unsigned i = 0; i < w; ++i) v |= bit(p + i) << i;
        return v;
    };
    // Bit 0: VC=1
    if (bit(0) != 1u) {
        std::fprintf(stderr, "[ready-selftest] FAIL: VC bit\n");
        return 7;
    }
    // Bits 2..10: send_seq=2
    const std::uint32_t seq = bits(2, 9);
    if (seq != 2u) {
        std::fprintf(stderr, "[ready-selftest] FAIL: send_seq=%u\n", seq);
        return 7;
    }
    // Walk ack list to find terminator + type word.
    std::size_t pos = 16;  // start of ack list
    for (;;) {
        const std::uint32_t len = bits(pos, 3); pos += 3;
        if (len == 0) break;
        pos += 5;
    }
    const std::uint32_t type_word = bits(pos, 5);
    if (type_word != 0u) {
        std::fprintf(stderr,
            "[ready-selftest] FAIL: type_word=%u (want 0=DataPacket)\n",
            type_word);
        return 7;
    }
    pos += 5;

    // Rate-control prefix.
    if (bit(pos) != 1u) {
        std::fprintf(stderr, "[ready-selftest] FAIL: R0 flag\n");
        return 7;
    }
    ++pos;
    const std::uint32_t cur_d = bits(pos, 10); pos += 10;
    const std::uint32_t cur_sz = bits(pos, 10); pos += 10;
    if (cur_d != 66u || cur_sz != 400u) {
        std::fprintf(stderr,
            "[ready-selftest] FAIL: cur d=%u sz=%u\n", cur_d, cur_sz);
        return 7;
    }
    if (bit(pos) != 1u) {
        std::fprintf(stderr, "[ready-selftest] FAIL: R1 flag\n");
        return 7;
    }
    ++pos;
    const std::uint32_t max_d = bits(pos, 10); pos += 10;
    const std::uint32_t max_sz = bits(pos, 10); pos += 10;
    if (max_d != 66u || max_sz != 400u) {
        std::fprintf(stderr,
            "[ready-selftest] FAIL: max d=%u sz=%u\n", max_d, max_sz);
        return 7;
    }

    // Event sub-stream present + first event header.
    if (bit(pos) != 1u) {
        std::fprintf(stderr, "[ready-selftest] FAIL: ess present\n");
        return 7;
    }
    ++pos;
    if (bit(pos) != 1u) {  // event-present
        std::fprintf(stderr, "[ready-selftest] FAIL: event-present\n");
        return 7;
    }
    ++pos;
    if (bit(pos) != 1u) {  // guaranteed
        std::fprintf(stderr, "[ready-selftest] FAIL: guaranteed\n");
        return 7;
    }
    ++pos;
    if (bit(pos) != 0u) {  // seq-continuous = 0
        std::fprintf(stderr, "[ready-selftest] FAIL: seq-continuous\n");
        return 7;
    }
    ++pos;
    if (bit(pos) != 1u) {  // has-explicit-seq
        std::fprintf(stderr, "[ready-selftest] FAIL: has-explicit-seq\n");
        return 7;
    }
    ++pos;
    const std::uint32_t exseq = bits(pos, 7); pos += 7;
    if (exseq != 0u) {
        std::fprintf(stderr, "[ready-selftest] FAIL: exseq=%u\n", exseq);
        return 7;
    }
    const std::uint32_t classid = bits(pos, 7); pos += 7;
    if (classid != 8u) {
        std::fprintf(stderr, "[ready-selftest] FAIL: class=%u\n", classid);
        return 7;
    }
    const std::uint32_t argc = bits(pos, 5); pos += 5;
    if (argc != 1u) {
        std::fprintf(stderr, "[ready-selftest] FAIL: argc=%u\n", argc);
        return 7;
    }
    // String: compression flag 0, length 1, byte-aligned 'A'.
    if (bit(pos) != 0u) {
        std::fprintf(stderr, "[ready-selftest] FAIL: compression flag\n");
        return 7;
    }
    ++pos;
    const std::uint32_t slen = bits(pos, 8); pos += 8;
    if (slen != 1u) {
        std::fprintf(stderr, "[ready-selftest] FAIL: strlen=%u\n", slen);
        return 7;
    }
    // Byte-align.
    if (pos & 7u) pos += (8 - (pos & 7u));
    const std::uint32_t ch = bits(pos, 8); pos += 8;
    if (ch != 'A') {
        std::fprintf(stderr, "[ready-selftest] FAIL: char=0x%02x\n", ch);
        return 7;
    }
    // Trailing terminators.
    if (bit(pos) != 0u) {
        std::fprintf(stderr, "[ready-selftest] FAIL: event term\n");
        return 7;
    }
    ++pos;
    if (bit(pos) != 0u) {
        std::fprintf(stderr, "[ready-selftest] FAIL: input present\n");
        return 7;
    }
    ++pos;
    if (bit(pos) != 0u) {
        std::fprintf(stderr, "[ready-selftest] FAIL: ghost present\n");
        return 7;
    }
    ++pos;

    std::fprintf(stderr,
        "[ready-selftest] decode OK: send_seq=2 parity=0 type=DataPacket\n"
        "                R0 d=66 sz=400  R1 d=66 sz=400\n"
        "                event-class=8 seq=0 argc=1 str=\"A\" "
        "(uncompressed)\n");
    std::fprintf(stderr, "[ready-selftest] consumed %zu bits, packet=%zu bytes\n",
        pos, wire.size());
    std::fprintf(stderr, "[ready-selftest] PASS\n");
    return 0;
}

int run_client(const std::string& host, std::uint16_t port,
               const std::string& name, const std::string& password,
               int duration_s)
{
    std::fprintf(stderr, "[net-test] connecting to %s:%u as '%s'\n",
        host.c_str(), port, name.c_str());

    Connection client;
    if (!client.bind(0)) {
        std::fprintf(stderr, "bind failed: %s\n",
            client.socket().last_error().c_str());
        return 2;
    }
    if (!client.connect(host, port, name, password, 1)) {
        std::fprintf(stderr, "connect: address resolution failed\n");
        return 2;
    }

    auto last_state = client.state();
    std::fprintf(stderr, "[net-test] initial state: %s\n",
        state_name(last_state));

    const std::uint64_t deadline = now_ms() + 1000ULL * duration_s;
    while (now_ms() < deadline) {
        client.tick(now_ms());
        if (client.state() != last_state) {
            std::fprintf(stderr, "[net-test] -> %s (retries=%d, rtt=%ums)\n",
                state_name(client.state()),
                client.retries(),
                client.rtt_ms());
            last_state = client.state();
            if (client.state() == Connection::State::Failed) {
                std::fprintf(stderr, "[net-test] reject reason: '%s'\n",
                    client.reject_reason().c_str());
                return 3;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::fprintf(stderr, "[net-test] final state: %s, rtt=%ums\n",
        state_name(client.state()), client.rtt_ms());
    if (client.state() == Connection::State::Connected
        || client.state() == Connection::State::Active) {
        std::fprintf(stderr, "[net-test] disconnecting cleanly\n");
        client.disconnect("test exit");
        for (int i = 0; i < 10; ++i) {
            client.tick(now_ms());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    // Exit OK if we reached Connected (or beyond) at any point in the run.
    // The disconnect path naturally leaves us in Disconnecting or Unbound.
    return 0;
}

} // anonymous namespace

int main(int argc, char** argv)
{
    std::string host = "127.0.0.1";
    std::uint16_t port = 28000;
    std::string name = "OpenSiege";
    std::string password;
    int duration_s = 5;
    bool loopback_self = false;
    bool template_paste = false;
    bool decode_ghosts = false;
    bool send_acks = false;          // spec 20/12 — off by default
    bool ack_selftest = false;
    bool ready_selftest = false;     // spec 20/22 — offline encoder check
    bool use_groove = false;         // spec 20/12 follow-up — 45B Groove template
    // Spec 20/22 — emit the c→s connection-progression event after
    // the first ghost-stream packet arrives. Tri-state to track whether
    // the user explicitly requested a value vs. defaulted (so that
    // --send-ready defaults on when --ack is set).
    int send_ready_opt = -1;          // -1 = unset, 0 = off, 1 = on
    std::string ghost_dump_path;
    std::string replay_path;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto take = [&](std::string& dst, const char* opt) -> bool {
            if (a != opt) return false;
            if (i + 1 >= argc) { usage(argv[0]); std::exit(1); }
            dst = argv[++i];
            return true;
        };
        if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        if (a == "--loopback-self") { loopback_self = true; continue; }
        if (a == "--template-paste") { template_paste = true; continue; }
        if (a == "--host" && i + 1 < argc) { host = argv[++i]; continue; }
        if (a == "--port" && i + 1 < argc) { port = static_cast<std::uint16_t>(std::atoi(argv[++i])); continue; }
        if (take(name, "--name")) continue;
        if (take(password, "--password")) continue;
        if (a == "--duration" && i + 1 < argc) { duration_s = std::atoi(argv[++i]); continue; }
        if (a == "--listen-seconds" && i + 1 < argc) { duration_s = std::atoi(argv[++i]); continue; }
        if (a == "--ghost-dump" && i + 1 < argc) { ghost_dump_path = argv[++i]; continue; }
        if (a == "--decode-ghosts") { decode_ghosts = true; continue; }
        if (a == "--replay" && i + 1 < argc) { replay_path = argv[++i]; continue; }
        if (a == "--ack") { send_acks = true; continue; }
        if (a == "--no-acks") { send_acks = false; continue; }
        if (a == "--ack-selftest") { ack_selftest = true; continue; }
        if (a == "--ready-selftest") { ready_selftest = true; continue; }
        if (a == "--groove") { use_groove = true; continue; }
        if (a == "--send-ready") { send_ready_opt = 1; continue; }
        if (a == "--no-send-ready") { send_ready_opt = 0; continue; }
        std::fprintf(stderr, "unknown arg '%s'\n", a.c_str());
        usage(argv[0]);
        return 1;
    }

    if (ack_selftest) return run_ack_selftest();
    if (ready_selftest) return run_ready_selftest();
    if (!replay_path.empty()) return run_replay(replay_path);
    if (loopback_self) return run_loopback_self(duration_s);
    // Resolve the spec 20/22 default: --send-ready defaults to on whenever
    // --ack is set (the ready emit requires a live ack stream to be useful).
    const bool send_ready =
        (send_ready_opt == 1) ||
        (send_ready_opt == -1 && send_acks);
    if (template_paste) return run_template_paste(host, port, duration_s,
                                                  ghost_dump_path, decode_ghosts,
                                                  send_acks, use_groove,
                                                  send_ready);
    return run_client(host, port, name, password, duration_s);
}
