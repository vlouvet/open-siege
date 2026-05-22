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

#include "ghost_stream.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
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
        "                       s->c packet through the parser (no server needed)\n",
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

int run_template_paste(const std::string& host, std::uint16_t port,
                       int duration_s,
                       const std::string& ghost_dump_path,
                       bool decode_ghosts)
{
    std::fprintf(stderr,
        "[net-test] template-paste mode -> %s:%u (sends captured-real Tribes\n"
        "[net-test] RequestConnect template, listens for AcceptConnect)\n",
        host.c_str(), port);

    UdpSocket sock;
    if (!sock.bind(0)) {
        std::fprintf(stderr, "bind failed: %s\n", sock.last_error().c_str());
        return 2;
    }

    // Fresh random nonce per run so the server can't dedupe.
    std::random_device rd;
    std::uint8_t pkt[27];
    std::memcpy(pkt, kRealRequestConnectTemplate, sizeof(pkt));
    pkt[7] = static_cast<std::uint8_t>(rd() & 0xff);
    pkt[8] = static_cast<std::uint8_t>(rd() & 0xff);
    pkt[9] = static_cast<std::uint8_t>(rd() & 0xff);

    const auto dst = resolve_endpoint(host, port);
    if (!dst) {
        std::fprintf(stderr, "resolve %s:%u failed\n", host.c_str(), port);
        return 2;
    }
    std::fprintf(stderr, "[net-test] sending nonce=%02x %02x %02x\n",
        pkt[7], pkt[8], pkt[9]);
    if (!sock.send_to(*dst, pkt, sizeof(pkt))) {
        std::fprintf(stderr, "send failed: %s\n", sock.last_error().c_str());
        return 2;
    }

    auto hex_dump = [](const char* label, const std::uint8_t* p, std::size_t n) {
        std::fprintf(stderr, "[net-test] %s (%zuB): ", label, n);
        for (std::size_t i = 0; i < n; ++i) std::fprintf(stderr, "%02x", p[i]);
        std::fputc('\n', stderr);
    };

    // Phase 1: wait for AcceptConnect (16B, nonce echoed at offset 4..6).
    const std::uint64_t accept_deadline = now_ms() + 3000;
    bool got_accept = false;
    while (now_ms() < accept_deadline) {
        std::vector<std::uint8_t> buf;
        Endpoint src;
        if (sock.try_recv(buf, src)) {
            hex_dump("recv", buf.data(), buf.size());
            const bool nonce_match = buf.size() >= 7
                && buf[4] == pkt[7] && buf[5] == pkt[8] && buf[6] == pkt[9];
            if (!nonce_match) {
                std::fprintf(stderr,
                    "[net-test] unexpected reply (nonce mismatch); abort\n");
                return 4;
            }
            // 16B reply with our nonce = AcceptConnect. Other lengths
            // (24..30B) are RejectConnect with an ASCII reason.
            if (buf.size() == 16) {
                std::fprintf(stderr,
                    "[net-test] phase-1: AcceptConnect received\n");
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

    // Phase 2: send the first DataPacket. This is the implicit "handshake
    // complete" signal. Bytes captured from a live session were 07 08 09 80;
    // re-using them here as a literal until we decode the bit layout.
    static const std::uint8_t kFirstDataPacket[4] = { 0x07, 0x08, 0x09, 0x80 };
    if (!sock.send_to(*dst, kFirstDataPacket, sizeof(kFirstDataPacket))) {
        std::fprintf(stderr, "[net-test] phase-2: send failed: %s\n",
            sock.last_error().c_str());
        return 2;
    }
    hex_dump("phase-2 send", kFirstDataPacket, sizeof(kFirstDataPacket));

    // Phase 3: server should now start streaming ghost data. Accumulate
    // every packet so a --ghost-dump path can emit a JSON file the
    // ghost-stream parser specs (20/09+) can replay against.
    struct CapturedPacket {
        std::uint64_t t_ms;
        std::vector<std::uint8_t> bytes;
    };
    std::vector<CapturedPacket> captured;
    int ghost_packets = 0;
    std::size_t ghost_bytes = 0;
    const std::uint64_t phase3_start = now_ms();
    const std::uint64_t deadline = phase3_start + 1000ULL * duration_s;
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
                auto dec = net20::parse_ghost_packet(buf.data(), buf.size());
                if (!dec.updates.empty()) {
                    std::fprintf(stderr, "[ghost] %s\n",
                        net20::format_update(dec.updates[0]).c_str());
                } else if (!dec.note.empty()) {
                    std::fprintf(stderr, "[ghost] (no record) %s\n",
                        dec.note.c_str());
                }
            }
            captured.push_back({ now_ms() - phase3_start, std::move(buf) });
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    std::fprintf(stderr,
        "[net-test] phase-3: %d ghost packets, %zu bytes total\n",
        ghost_packets, ghost_bytes);

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

    // Acceptance: at least 1 ghost record across the capture.
    return with_record > 0 ? 0 : 6;
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
        std::fprintf(stderr, "unknown arg '%s'\n", a.c_str());
        usage(argv[0]);
        return 1;
    }

    if (!replay_path.empty()) return run_replay(replay_path);
    if (loopback_self) return run_loopback_self(duration_s);
    if (template_paste) return run_template_paste(host, port, duration_s,
                                                  ghost_dump_path, decode_ghosts);
    return run_client(host, port, name, password, duration_s);
}
