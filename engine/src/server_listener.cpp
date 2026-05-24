#include <osengine/server_listener.hpp>

#include "content/net/udp_socket.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
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

// 45-byte Groove RequestConnect: starts with 0x05 (TribesNext shape).
bool looks_like_groove_request_connect(const std::vector<std::uint8_t>& pkt)
{
    return pkt.size() == 45 && pkt[0] == 0x05;
}

// 4-byte DataPacket / first ack (0x07 0x08 0x09 0x80 per BREAKTHROUGH).
bool looks_like_first_data_packet(const std::vector<std::uint8_t>& pkt)
{
    return pkt.size() == 4 && pkt[0] == 0x07 && pkt[1] == 0x08;
}

void hex_prefix(const std::vector<std::uint8_t>& pkt, char* buf, std::size_t bufsz)
{
    const std::size_t n = std::min(pkt.size(), std::size_t{16});
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

struct ServerListener::Impl
{
    studio::content::net::UdpSocket socket;
    mutable std::mutex              mu;
    ServerListenerStats             stats;
    std::string                     last_error;
};

ServerListener::ServerListener(ServerListenerConfig cfg)
    : cfg_(cfg), impl_(std::make_unique<Impl>())
{}

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

void ServerListener::run()
{
    using namespace std::chrono;
    using studio::content::net::Endpoint;

    const auto period = milliseconds(1000 / std::max(1, cfg_.tick_hz));
    std::vector<std::uint8_t> buf;
    Endpoint peer;
    std::uint8_t reply[16];

    while (!quit_.load()) {
        const auto t0 = steady_clock::now();

        // Drain inbound queue. try_recv returns false when queue empty.
        while (impl_->socket.try_recv(buf, peer)) {
            if (looks_like_vanilla_request_connect(buf)) {
                const std::uint8_t nonce[3] = { buf[7], buf[8], buf[9] };
                build_accept_connect_reply(nonce, reply);
                const bool ok = impl_->socket.send_to(peer, reply, sizeof(reply));
                std::lock_guard<std::mutex> lk(impl_->mu);
                ++impl_->stats.request_connects_received;
                if (ok) {
                    ++impl_->stats.accept_connects_sent;
                    std::fprintf(stderr,
                        "[listener] RequestConnect from %s:%u, replied AcceptConnect (nonce %02x%02x%02x)\n",
                        peer.host.c_str(), peer.port, nonce[0], nonce[1], nonce[2]);
                } else {
                    impl_->last_error = impl_->socket.last_error();
                    std::fprintf(stderr,
                        "[listener] send_to %s:%u failed: %s\n",
                        peer.host.c_str(), peer.port, impl_->last_error.c_str());
                }
            }
            else if (looks_like_groove_request_connect(buf)) {
                std::lock_guard<std::mutex> lk(impl_->mu);
                ++impl_->stats.unknown_packets_received;
                std::fprintf(stderr,
                    "[listener] Groove RequestConnect from %s:%u (45B, ignored — v1 vanilla only)\n",
                    peer.host.c_str(), peer.port);
            }
            else if (looks_like_first_data_packet(buf)) {
                // Spec 26/11: reply with the captured 223-byte ghost burst
                // so the client's phase-3 counter ticks up. v1 sends the
                // burst on every observed DataPacket from this peer; spec
                // 26/12 will track per-peer state and only send once.
                const bool ok = impl_->socket.send_to(
                    peer, kFirstGhostBurstTemplate, sizeof(kFirstGhostBurstTemplate));
                std::lock_guard<std::mutex> lk(impl_->mu);
                ++impl_->stats.data_packets_received;
                if (ok) {
                    ++impl_->stats.ghost_bursts_sent;
                    std::fprintf(stderr,
                        "[listener] DataPacket %02x%02x%02x%02x from %s:%u, replied ghost burst (%zuB)\n",
                        buf[0], buf[1], buf[2], buf[3], peer.host.c_str(), peer.port,
                        sizeof(kFirstGhostBurstTemplate));
                } else {
                    impl_->last_error = impl_->socket.last_error();
                    std::fprintf(stderr,
                        "[listener] DataPacket from %s:%u, send_to(ghost burst) failed: %s\n",
                        peer.host.c_str(), peer.port, impl_->last_error.c_str());
                }
            }
            else {
                char hexbuf[64] = {};
                hex_prefix(buf, hexbuf, sizeof(hexbuf));
                std::lock_guard<std::mutex> lk(impl_->mu);
                ++impl_->stats.unknown_packets_received;
                std::fprintf(stderr,
                    "[listener] unknown %zuB from %s:%u: %s\n",
                    buf.size(), peer.host.c_str(), peer.port, hexbuf);
            }
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

    ServerListener listener(ServerListenerConfig{listener_port, 200});
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
    ServerListener listener2(ServerListenerConfig{listener_port, 200});
    if (!listener2.start()) {
        std::fprintf(stderr, "[listener-selftest] phase2 listener.start failed: %s\n",
                     listener2.last_error().c_str());
        return 1;
    }
    Endpoint target2{"127.0.0.1", listener_port};
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

    std::fprintf(stderr, "[listener-selftest] OK — handshake + ghost burst both byte-equal\n");
    return 0;
}

} // namespace dts_viewer
