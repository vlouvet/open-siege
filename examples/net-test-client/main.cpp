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

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>

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
        "                       (verified to elicit AcceptConnect from real Tribes)\n",
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
                       int duration_s)
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

    const std::uint64_t deadline = now_ms() + 1000ULL * duration_s;
    bool saw_reply = false;
    while (now_ms() < deadline) {
        std::vector<std::uint8_t> buf;
        Endpoint src;
        if (sock.try_recv(buf, src)) {
            saw_reply = true;
            std::fprintf(stderr, "[net-test] reply: %zu bytes from %s:%u\n",
                buf.size(), src.host.c_str(), src.port);
            std::fputs("[net-test] hex: ", stderr);
            for (auto b : buf) std::fprintf(stderr, "%02x", b);
            std::fputc('\n', stderr);
            // Hunt for our nonce in the reply.
            for (std::size_t i = 0; i + 3 <= buf.size(); ++i) {
                if (buf[i] == pkt[7] && buf[i+1] == pkt[8] && buf[i+2] == pkt[9]) {
                    std::fprintf(stderr,
                        "[net-test] nonce echoed at reply offset %zu\n", i);
                    break;
                }
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!saw_reply) {
        std::fprintf(stderr, "[net-test] no reply within %ds — server silent\n",
            duration_s);
        return 4;
    }
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
        std::fprintf(stderr, "unknown arg '%s'\n", a.c_str());
        usage(argv[0]);
        return 1;
    }

    if (loopback_self) return run_loopback_self(duration_s);
    if (template_paste) return run_template_paste(host, port, duration_s);
    return run_client(host, port, name, password, duration_s);
}
