#ifndef OSENGINE_SERVER_LISTENER_HPP
#define OSENGINE_SERVER_LISTENER_HPP

// UDP listener for open-siege-t1-server — spec 26/10.
//
// Binds a UDP port, drains inbound packets in a tick thread, replies
// to vanilla Tribes 1.41 RequestConnect (27 bytes, byte[0]==0x07) with
// the captured-template AcceptConnect (16 bytes, nonce echoed at
// bytes 4..6).
//
// v1 scope: handshake only. DataPacket / acks / ghost-burst response
// are spec 26/11 follow-up.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace dts_viewer
{

struct ServerListenerConfig
{
    std::uint16_t port    = 28000;
    int           tick_hz = 32;
};

struct ServerListenerStats
{
    std::uint64_t request_connects_received = 0;
    std::uint64_t accept_connects_sent      = 0;
    std::uint64_t data_packets_received     = 0;
    std::uint64_t unknown_packets_received  = 0;
};

class ServerListener
{
public:
    explicit ServerListener(ServerListenerConfig cfg);
    ~ServerListener();

    ServerListener(const ServerListener&) = delete;
    ServerListener& operator=(const ServerListener&) = delete;

    // Bind + spawn the tick thread. Returns false (and populates
    // last_error()) if the bind failed.
    bool start();

    // Stop the thread, close the socket, join. Idempotent.
    void stop();

    bool                running()   const { return running_.load(); }
    ServerListenerStats stats()     const;
    std::string         last_error() const;

private:
    void run();

    ServerListenerConfig cfg_;
    std::atomic<bool>    running_{false};
    std::atomic<bool>    quit_{false};
    std::thread          thread_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Build the 16-byte AcceptConnect reply for a given 3-byte nonce.
// Exposed so the self-test can verify byte-for-byte equality without
// needing a socket.
void build_accept_connect_reply(const std::uint8_t nonce[3],
                                std::uint8_t out[16]);

// Run a synchronous in-process selftest: spawn a ServerListener bound
// on an ephemeral port, send a synthetic RequestConnect, assert the
// reply is correct. Returns 0 on success, non-zero on any failure.
int server_listener_selftest();

} // namespace dts_viewer

#endif // OSENGINE_SERVER_LISTENER_HPP
