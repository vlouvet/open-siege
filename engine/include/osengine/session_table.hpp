#ifndef OSENGINE_SESSION_TABLE_HPP
#define OSENGINE_SESSION_TABLE_HPP

// Per-peer session tracking for the open-siege server — spec 28/01.
//
// Each accepted client gets a Session keyed by its UDP Endpoint. The
// session carries enough state for the listener to:
//   * reply to a retransmitted RequestConnect with the original nonce,
//   * gate the ghost-burst send to once per session lifecycle (rather
//     than once per inbound DataPacket as in spec 26/11),
//   * time out clients whose ack stream goes silent for >5 s,
//   * allocate a unique player_slot in [0, max_players).
//
// Subsequent specs (28/02 movecommand ingestion, 28/03 server player
// tick, 28/04 ghost emission, 28/05 spawn+teams) extend Session with
// gameplay fields. Keep this header lean.

#include "content/net/udp_socket.hpp"
#include <osengine/movecommand.hpp>
#include <osengine/player_controller.hpp>
#include <osengine/reliable_acks.hpp>

#include <cstdint>
#include <deque>
#include <glm/glm.hpp>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace dts_viewer
{

// Spec 28/05 — team membership. Spectator means the session is
// connected but not placed on the map; deathmatch missions still
// assign Red/Blue for color bookkeeping.
enum class Team : std::uint8_t {
    Spectator = 0,
    Red       = 1,
    Blue      = 2,
};

struct Session
{
    studio::content::net::Endpoint peer;
    std::uint8_t  nonce[3] = {0, 0, 0};   // echoed in AcceptConnect
    std::uint16_t player_slot = 0;         // 0..max_players-1
    std::uint64_t connected_at_ms = 0;
    std::uint64_t last_seen_ms = 0;
    std::uint16_t next_send_seq = 1;       // server replies start at seq 1
    std::uint16_t last_acked_recv_seq = 0;
    bool          ghost_burst_sent = false;
    // 26/10b follow-up — set on session allocate when the inbound
    // RequestConnect was a TAH (Groove/TribesNext) shape. Drives the
    // canned-burst selection: TAH gets the captured TAH-server burst
    // (10 packets / 2228B), vanilla gets the 223B vanilla burst.
    bool          is_tah_session = false;
    // 26/14a — per-session ack-window over inbound seqs from this peer,
    // and timestamps for the outbound pure-ack tick.
    net20::AckTracker ack;
    bool          connect_parity = false;     // last AC parity bit; echoed
                                              // in our outbound headers
    // 26/14c — full 32-bit per-session connect handle, copied from the
    // client's RequestConnect body. Used to (a) populate `connect_parity`
    // (its LSB) and (b) embed in the body of the four connection-control
    // packet types (Accept/Reject/Disconnect/RequestConnect) per the
    // TRIBES-VC-OUTBOUND clean-room spec §2. Zero by default for sessions
    // built by paths that pre-date this field (handshake selftests, etc.).
    std::uint32_t connect_handle = 0;
    std::uint64_t last_outbound_ms = 0;       // for 250ms keep-alive tick

    // Spec 28/02 — pending input from this peer. Drained by the per-tick
    // world step (spec 28/03). Bounded by sanity at ~64 entries.
    std::deque<net20::MoveInput> pending_moves;
    std::uint32_t                last_applied_move_seq = 0;

    // Spec 28/03 — server-authoritative player state for this session.
    // Advanced by world_tick(); ghost-emitted to clients in spec 28/04.
    PlayerState                  player_state{};

    // Spec 28/05 — team + spawn placement. Filled in by team_assigner
    // on session allocate; spawn_pos is also written into
    // player_state.pos so the first world_tick sees the player at the
    // chosen drop point.
    Team       team         = Team::Spectator;
    glm::vec3  spawn_pos    {0.0f, 0.0f, 0.0f};
    float      spawn_yaw    = 0.0f;

    // Spec 28/07 — life-state + scoring.
    enum class LifeState : std::uint8_t { Alive = 0, Dead = 1 };
    LifeState     life          = LifeState::Alive;
    std::uint64_t died_at_ms    = 0;
    std::uint16_t kills         = 0;
    std::uint16_t deaths        = 0;
    std::uint16_t last_killer   = 0xFFFF;
};

class SessionTable
{
public:
    explicit SessionTable(std::uint16_t max_players);

    // Allocate a session for `peer`. If the peer is already present
    // (a RequestConnect retransmit) returns the existing session
    // unchanged. Returns nullptr if max_players reached and the peer
    // is not yet in the table — caller should send RejectConnect.
    Session* allocate(const studio::content::net::Endpoint& peer,
                      const std::uint8_t nonce[3],
                      std::uint64_t now_ms);

    // Lookup by endpoint (host + port). Returns nullptr if not present.
    Session* find(const studio::content::net::Endpoint& peer);

    // Drop a single peer (e.g. on explicit disconnect). No-op if absent.
    void drop(const studio::content::net::Endpoint& peer);

    // Reap sessions whose last_seen_ms < now_ms - timeout_ms. Returns
    // the number dropped. Caller can iterate the drop list via the
    // out_dropped vector (pushed in slot order).
    std::size_t reap(std::uint64_t now_ms,
                     std::uint64_t timeout_ms,
                     std::vector<Session>* out_dropped = nullptr);

    // Touch last_seen_ms for a session (call on every received packet).
    void touch(const studio::content::net::Endpoint& peer,
               std::uint64_t now_ms);

    std::size_t size() const;
    std::uint16_t max_players() const { return max_players_; }

    // For ghost-emission iteration (spec 28/04 will use this). The
    // returned pointers stay valid until the next allocate/drop/reap
    // call on the same thread.
    std::vector<Session*> active_sessions();

private:
    struct EndpointHash {
        std::size_t operator()(const studio::content::net::Endpoint& e) const noexcept
        {
            std::size_t h = std::hash<std::string>{}(e.host);
            return h ^ (std::size_t(e.port) * 0x9E3779B97F4A7C15ull);
        }
    };
    struct EndpointEq {
        bool operator()(const studio::content::net::Endpoint& a,
                        const studio::content::net::Endpoint& b) const noexcept
        {
            return a.port == b.port && a.host == b.host;
        }
    };

    mutable std::mutex                                       mu_;
    std::uint16_t                                            max_players_;
    std::vector<bool>                                        slot_used_;
    std::unordered_map<studio::content::net::Endpoint,
                       Session,
                       EndpointHash,
                       EndpointEq>                           by_peer_;

    std::uint16_t alloc_slot_locked();
    void          free_slot_locked(std::uint16_t slot);
};

} // namespace dts_viewer

#endif // OSENGINE_SESSION_TABLE_HPP
