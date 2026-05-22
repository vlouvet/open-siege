#ifndef LIB3SPACE_NET_CONNECTION_HPP
#define LIB3SPACE_NET_CONNECTION_HPP

// Track 20 spec 02 — VC connection state machine.
//
// Source: docs/clean-room-specs/TRIBES-NETPROTO.md §§3-4.
//
// Drives the client-side handshake from Unbound -> RequestingConnection
// -> Connected -> Active (Active happens once the first server snapshot
// containing scope-always objects has been received; that part is the
// ghost-stream's job — see Track 20 spec 04).
//
// Usage:
//
//   Connection conn;
//   conn.bind(0);                  // ephemeral source port
//   conn.connect("127.0.0.1", 28000, "OpenSiege", "");
//   while (conn.state() != Connection::State::Active
//          && conn.state() != Connection::State::Failed) {
//       conn.tick(now_ms);
//   }
//
// The connection emits its own RequestConnect / keepalive / ping /
// retransmits via the underlying UdpSocket. Application code reads
// reject reasons via reject_reason() and live state via state().

#include "content/net/udp_socket.hpp"
#include "content/net/vc_packet.hpp"

#include <cstdint>
#include <string>

namespace studio::content::net
{

class Connection
{
public:
    enum class State
    {
        Unbound,
        RequestingConnection,   // client sent RequestConnect, waiting
        AcceptingConnection,    // server side: sent AcceptConnect, waiting for first DataPacket
        Connected,              // both sides ack'd; engine-level still loading
        Active,                 // first server snapshot received + acked
        Disconnecting,
        Failed,                 // rejected or timed out
    };

    // Default ctor — caller must call bind() before connect()/listen().
    Connection() = default;
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    bool bind(std::uint16_t local_port);

    // --- Client side --------------------------------------------------------

    // Begin a connect handshake. Transitions Unbound -> RequestingConnection.
    // `password` is empty for unprotected servers.
    bool connect(const std::string& server_host,
                 std::uint16_t server_port,
                 const std::string& player_name,
                 const std::string& password,
                 std::uint16_t protocol_version = 1);

    // --- Server side --------------------------------------------------------

    // Begin listening for client RequestConnects on the bound port. State
    // stays Unbound until a client arrives.
    void listen();

    // Per-tick: pump received datagrams, run retries, advance state.
    // `now_ms` is a monotonic millisecond clock from the application.
    void tick(std::uint64_t now_ms);

    // --- Application-facing -------------------------------------------------

    void disconnect(const std::string& reason = "");

    State                state() const { return state_; }
    const Endpoint&      peer()  const { return peer_; }
    std::uint32_t        connect_sequence() const { return connect_sequence_; }
    std::uint32_t        assigned_client_id() const { return assigned_client_id_; }
    const std::string&   reject_reason() const { return reject_reason_; }
    const std::string&   welcome_string() const { return welcome_string_; }
    std::uint32_t        rtt_ms() const { return static_cast<std::uint32_t>(rtt_avg_ms_); }

    // Diagnostics.
    int                  retries() const { return retry_count_; }
    bool                 is_open() const { return sock_.is_open(); }
    UdpSocket&           socket() { return sock_; }   // for tests

    // Promote Connected -> Active. Higher layers (ghost stream) call this
    // once the first scope-always snapshot has been received + acked.
    void mark_active();

private:
    enum class Role { Client, Server };

    void send_request_connect();
    void send_accept_connect();
    void send_reject_connect(const std::string& reason);
    void send_disconnect(const std::string& reason);
    void send_ping();
    void handle_datagram(const std::uint8_t* data, std::size_t size,
                         const Endpoint& from, std::uint64_t now_ms);
    void handle_request_connect(const VCHeader& hdr, BitStream& s,
                                const Endpoint& from, std::uint64_t now_ms);
    void handle_accept_connect(const VCHeader& hdr, BitStream& s,
                               std::uint64_t now_ms);
    void handle_reject_connect(const VCHeader& hdr, BitStream& s);
    void handle_disconnect(const VCHeader& hdr, BitStream& s);

    void maybe_retransmit(std::uint64_t now_ms);
    std::uint64_t next_retry_delay_ms() const;

    UdpSocket    sock_;
    Role         role_ = Role::Client;
    State        state_ = State::Unbound;
    Endpoint     peer_;

    std::string  player_name_;
    std::string  password_;
    std::uint16_t protocol_version_ = 1;

    std::uint32_t connect_sequence_ = 0;
    std::uint32_t assigned_client_id_ = 0;
    std::string  reject_reason_;
    std::string  welcome_string_;

    std::uint16_t send_seq_ = 0;
    std::uint8_t  highest_recvd_seq_ = 0;
    std::uint64_t last_sent_ms_ = 0;
    std::uint64_t last_recv_ms_ = 0;
    int           retry_count_ = 0;
    float         rtt_avg_ms_ = 100.0f;
};

const char* state_name(Connection::State s);

} // namespace studio::content::net

#endif // LIB3SPACE_NET_CONNECTION_HPP
