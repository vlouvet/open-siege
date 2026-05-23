#ifndef OSENGINE_LISTEN_SERVER_HPP
#define OSENGINE_LISTEN_SERVER_HPP

// Embedded server thread for SP — spec 26/07.
//
// Spawns a background thread that runs the same fixed-step tick loop
// open-siege-t1-server uses, and exposes a LoopbackEndpoint the
// in-process client can talk to instead of UDP. This unifies SP and MP
// behind a single engine path: SP is just "MP with the server running
// in the same process and an in-process transport".
//
// v1 scope: the tick thread spins at the configured rate, drains any
// inbound client packets on the loopback (logs them), and exits cleanly
// on stop(). Full mission-state replication is a follow-up — for the
// stub binaries today this proves the threading + queue plumbing works.

#include <osengine/loopback_transport.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

namespace dts_viewer
{

struct ListenServerConfig
{
    int          tick_hz       = 32;
    std::uint64_t max_ticks    = 0;   // 0 = run until stop()
};

class ListenServer
{
public:
    explicit ListenServer(ListenServerConfig cfg);
    ~ListenServer();

    ListenServer(const ListenServer&) = delete;
    ListenServer& operator=(const ListenServer&) = delete;

    // Start the tick thread. Idempotent (no-op if already running).
    void start();

    // Stop the thread, drain queues, join. Safe to call multiple times.
    void stop();

    // Client-side endpoint of the loopback. Hand this to the in-process
    // client so it can send/recv messages exactly like a UDP NetClient.
    LoopbackEndpoint& client_endpoint() { return pair_.client; }

    bool          running() const { return running_.load(); }
    std::uint64_t ticks() const   { return ticks_.load(); }

private:
    void run();

    ListenServerConfig     cfg_;
    LoopbackPair           pair_;
    std::thread            thread_;
    std::atomic<bool>      running_{false};
    std::atomic<bool>      quit_{false};
    std::atomic<std::uint64_t> ticks_{0};
};

} // namespace dts_viewer

#endif // OSENGINE_LISTEN_SERVER_HPP
