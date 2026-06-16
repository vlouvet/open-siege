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
#include <vector>

namespace dts_viewer { class SessionTable; }

namespace dts_viewer
{

struct ServerListenerConfig
{
    std::uint16_t port         = 28000;
    int           tick_hz      = 32;
    std::uint16_t max_players  = 32;          // spec 28/01
    std::uint64_t session_timeout_ms = 30000; // spec 28/01 (bumped from
                                              // 5s -> 30s in 26/10b
                                              // follow-up so TAH clients
                                              // get time to chew the
                                              // 8.8kB burst)
    bool          enable_ghost_emit = true;   // spec 28/04 — OSGB stream
    bool          enable_canned_burst = true; // spec 26/11 backstop
    int           ghost_emit_tick_div = 2;    // emit every Nth tick (32/2 = 16 Hz)
    bool          team_balance = true;        // spec 28/05 round-robin
};

struct ServerListenerStats
{
    std::uint64_t request_connects_received = 0;
    std::uint64_t accept_connects_sent      = 0;
    std::uint64_t reject_connects_sent      = 0;     // spec 28/01
    std::uint64_t data_packets_received     = 0;
    std::uint64_t ghost_bursts_sent         = 0;
    std::uint64_t unknown_packets_received  = 0;
    std::uint64_t sessions_active           = 0;     // spec 28/01
    std::uint64_t sessions_dropped          = 0;     // spec 28/01
    std::uint64_t movecommands_received     = 0;     // spec 28/02
    std::uint64_t malformed_movecommands    = 0;     // spec 28/02
    std::uint64_t ghost_emit_packets        = 0;     // spec 28/04
    std::uint64_t ghost_emit_records        = 0;     // spec 28/04
    std::uint64_t ghost_emit_bytes          = 0;     // spec 28/04
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

    // Spec 28/03 — the server's tick loop reads this to drive
    // world_tick(); spec 28/04's emitter reads it to iterate ghosts.
    // The listener thread mutates pending_moves under its own internal
    // serialization; callers must only invoke this from the same
    // logical "tick thread" (which IS the listener thread today —
    // spec 28/03 calls world_tick from inside the listener's tick).
    SessionTable&       sessions();

    // Spec 28/05 — the listener applies a spawn-point list to every
    // newly-allocated session. Caller sets this once after construction;
    // an empty list disables placement (sessions stay at origin).
    void set_spawn_points(std::vector<struct SpawnPoint> spawns);

    // Spec 28/07 — main loop reads this so it can call respawn_due
    // each tick with the current spawn list.
    const std::vector<struct SpawnPoint>& spawn_points() const;

    // Spec 29/02b — current mission name; published to clients in the
    // server_info packet sent after each new-session AcceptConnect.
    void set_mission_name(std::string name);

    // 14c-I-7 followup — loaded mission pointer passed through to the
    // TAH burst orchestrator. Without this the orchestrator falls back
    // to stub scope-always objects, which TAH rejects. Lifetime is the
    // caller's responsibility; passing nullptr unsets it.
    void set_loaded_mission(const struct LoadedMission* mission);

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

// Spec 26/10b — 18-byte Groove AcceptConnect builder. Echoes the
// 3-byte per-session nonce at offsets 4..6, identical to vanilla.
void build_groove_accept_connect_reply(const std::uint8_t nonce[3],
                                       std::uint8_t out[18]);

// 18-byte TribesAfterHope AcceptConnect builder (53B RC handler).
// Per-session nonce at reply offsets 4..6; byte 8 is a parity bit
// derived from request byte 10 (the parity byte that TAH alternates
// across retransmits).
void build_tah_accept_connect_reply(const std::uint8_t nonce[3],
                                    std::uint8_t      request_parity_byte,
                                    std::uint8_t      out[18]);

// Spec 26/10b — round-trip selftest: replay the captured Groove
// RequestConnect, assert the listener responds with the correct
// 18-byte AcceptConnect with the nonce echoed back, and confirm a
// session was allocated. Returns 0 on success.
int groove_handshake_selftest();

// Run a synchronous in-process selftest: spawn a ServerListener bound
// on an ephemeral port, send a synthetic RequestConnect, assert the
// reply is correct. Returns 0 on success, non-zero on any failure.
int server_listener_selftest();

} // namespace dts_viewer

#endif // OSENGINE_SERVER_LISTENER_HPP
