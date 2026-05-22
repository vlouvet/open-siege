#include "content/net/connection.hpp"

#include <chrono>
#include <cstdio>
#include <random>

namespace studio::content::net
{

namespace
{
constexpr std::uint64_t kMaxRetries          = 15;
constexpr std::uint64_t kRetryBaseMs         = 3000;   // (retry+1) * 3000 ms
constexpr std::uint64_t kKeepaliveIdleMs     = 3000;   // ping if idle longer
constexpr std::size_t   kSendBufBytes        = 1500;
constexpr std::uint16_t kSendSeqMask         = 0x1FF;  // 9 bits
constexpr std::uint8_t  kRecvSeqMask         = 0x1F;   // 5 bits

std::uint32_t random_u32()
{
    static std::random_device rd;
    static std::mt19937_64 mt(rd());
    std::uniform_int_distribution<std::uint32_t> dist;
    return dist(mt);
}

bool send_packet(UdpSocket& sock, const Endpoint& peer, const VCHeader& hdr,
                 const std::uint8_t* body, std::size_t body_size)
{
    std::uint8_t buf[kSendBufBytes];
    BitStream s(buf, sizeof(buf));
    if (!write_vc_header(s, hdr)) return false;
    if (body && body_size > 0) {
        s.write_bytes(body, body_size);
    }
    if (!s.is_valid()) return false;
    return sock.send_to(peer, buf, s.byte_position());
}

template <typename Body>
bool send_control_packet(UdpSocket& sock, const Endpoint& peer,
                         VCHeader& hdr, const Body& body,
                         bool (*body_writer)(BitStream&, const Body&))
{
    std::uint8_t buf[kSendBufBytes];
    BitStream s(buf, sizeof(buf));
    if (!write_vc_header(s, hdr)) return false;
    if (!body_writer(s, body)) return false;
    return sock.send_to(peer, buf, s.byte_position());
}

} // anonymous namespace

const char* state_name(Connection::State s)
{
    switch (s) {
        case Connection::State::Unbound:              return "Unbound";
        case Connection::State::RequestingConnection: return "RequestingConnection";
        case Connection::State::AcceptingConnection:  return "AcceptingConnection";
        case Connection::State::Connected:            return "Connected";
        case Connection::State::Active:               return "Active";
        case Connection::State::Disconnecting:        return "Disconnecting";
        case Connection::State::Failed:               return "Failed";
    }
    return "?";
}

Connection::~Connection() = default;

bool Connection::bind(std::uint16_t local_port)
{
    return sock_.bind(local_port);
}

bool Connection::connect(const std::string& server_host,
                         std::uint16_t server_port,
                         const std::string& player_name,
                         const std::string& password,
                         std::uint16_t protocol_version)
{
    auto ep = resolve_endpoint(server_host, server_port);
    if (!ep) return false;
    peer_ = *ep;
    role_ = Role::Client;
    player_name_ = player_name;
    password_ = password;
    protocol_version_ = protocol_version;
    connect_sequence_ = random_u32();
    send_seq_ = 0;
    highest_recvd_seq_ = 0;
    retry_count_ = 0;
    state_ = State::RequestingConnection;
    send_request_connect();
    return true;
}

void Connection::listen()
{
    role_ = Role::Server;
    state_ = State::Unbound;   // stays here until a RequestConnect arrives
}

void Connection::disconnect(const std::string& reason)
{
    if (state_ == State::Unbound || state_ == State::Failed) return;
    send_disconnect(reason);
    state_ = State::Disconnecting;
}

void Connection::mark_active()
{
    if (state_ == State::Connected) state_ = State::Active;
}

void Connection::send_request_connect()
{
    VCHeader hdr;
    hdr.discovery_flag = false;
    hdr.connect_seq_parity = (connect_sequence_ & 1u) != 0;
    hdr.send_seq = send_seq_++ & kSendSeqMask;
    hdr.highest_recvd_seq = highest_recvd_seq_ & kRecvSeqMask;
    hdr.type = PacketType::RequestConnect;
    hdr.connect_sequence = connect_sequence_;

    RequestConnectBody body;
    body.game_id = "TRIBES";
    body.protocol_version = protocol_version_;
    body.player_name = player_name_;
    body.password = password_;

    if (send_control_packet(sock_, peer_, hdr, body, &write_request_connect)) {
        last_sent_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}

void Connection::send_accept_connect()
{
    VCHeader hdr;
    hdr.connect_seq_parity = (connect_sequence_ & 1u) != 0;
    hdr.send_seq = send_seq_++ & kSendSeqMask;
    hdr.highest_recvd_seq = highest_recvd_seq_ & kRecvSeqMask;
    hdr.type = PacketType::AcceptConnect;
    hdr.connect_sequence = connect_sequence_;

    AcceptConnectBody body;
    body.assigned_client_id = assigned_client_id_;
    body.welcome_string = welcome_string_;

    send_control_packet(sock_, peer_, hdr, body, &write_accept_connect);
}

void Connection::send_reject_connect(const std::string& reason)
{
    VCHeader hdr;
    hdr.connect_seq_parity = (connect_sequence_ & 1u) != 0;
    hdr.send_seq = send_seq_++ & kSendSeqMask;
    hdr.highest_recvd_seq = highest_recvd_seq_ & kRecvSeqMask;
    hdr.type = PacketType::RejectConnect;
    hdr.connect_sequence = connect_sequence_;
    RejectConnectBody body{ reason };
    send_control_packet(sock_, peer_, hdr, body, &write_reject_connect);
}

void Connection::send_disconnect(const std::string& reason)
{
    VCHeader hdr;
    hdr.connect_seq_parity = (connect_sequence_ & 1u) != 0;
    hdr.send_seq = send_seq_++ & kSendSeqMask;
    hdr.highest_recvd_seq = highest_recvd_seq_ & kRecvSeqMask;
    hdr.type = PacketType::Disconnect;
    hdr.connect_sequence = connect_sequence_;
    DisconnectBody body{ reason };
    send_control_packet(sock_, peer_, hdr, body, &write_disconnect);
}

void Connection::send_ping()
{
    VCHeader hdr;
    hdr.connect_seq_parity = (connect_sequence_ & 1u) != 0;
    hdr.send_seq = send_seq_++ & kSendSeqMask;
    hdr.highest_recvd_seq = highest_recvd_seq_ & kRecvSeqMask;
    hdr.type = PacketType::Ping;
    send_packet(sock_, peer_, hdr, nullptr, 0);
}

std::uint64_t Connection::next_retry_delay_ms() const
{
    return static_cast<std::uint64_t>(retry_count_ + 1) * kRetryBaseMs;
}

void Connection::maybe_retransmit(std::uint64_t now_ms)
{
    if (last_sent_ms_ == 0) return;
    if (now_ms - last_sent_ms_ < next_retry_delay_ms()) return;

    if (retry_count_ >= static_cast<int>(kMaxRetries)) {
        reject_reason_ = "Connection timed out (no response after 15 retries)";
        state_ = State::Failed;
        return;
    }
    ++retry_count_;

    switch (state_) {
        case State::RequestingConnection:
            send_request_connect();
            break;
        case State::AcceptingConnection:
            send_accept_connect();
            break;
        case State::Connected:
        case State::Active:
            // Keepalive ping: same retry cadence, but doesn't retransmit
            // anything important — just a heartbeat.
            send_ping();
            last_sent_ms_ = now_ms;
            break;
        default:
            break;
    }
}

void Connection::tick(std::uint64_t now_ms)
{
    if (state_ == State::Unbound || state_ == State::Failed) {
        // Server-side listener still pumps recv even when Unbound.
        if (role_ != Role::Server || state_ == State::Failed) {
            // Drain any straggler packets so they don't pile up in the OS buffer.
            std::vector<std::uint8_t> data;
            Endpoint from;
            while (sock_.try_recv(data, from)) { /* discard */ }
            return;
        }
    }

    std::vector<std::uint8_t> data;
    Endpoint from;
    while (sock_.try_recv(data, from)) {
        handle_datagram(data.data(), data.size(), from, now_ms);
    }

    // Keepalive: when in Connected/Active and the link's been silent for a
    // while, send a Ping. This shares the same retry counter so an
    // unresponsive peer eventually trips the timeout.
    if ((state_ == State::Connected || state_ == State::Active)
        && last_sent_ms_ != 0
        && now_ms - last_sent_ms_ >= kKeepaliveIdleMs)
    {
        send_ping();
        last_sent_ms_ = now_ms;
    }

    maybe_retransmit(now_ms);
}

void Connection::handle_datagram(const std::uint8_t* data, std::size_t size,
                                 const Endpoint& from, std::uint64_t now_ms)
{
    BitStream s(data, size);
    auto hdr_opt = read_vc_header(s);
    if (!hdr_opt) return;
    const VCHeader& hdr = *hdr_opt;

    // Discovery datagrams travel through a different decoder. We ignore them
    // for now; the connection layer only cares about VC packets.
    if (hdr.discovery_flag) return;

    last_recv_ms_ = now_ms;
    highest_recvd_seq_ = hdr.send_seq & kRecvSeqMask;

    switch (hdr.type) {
        case PacketType::RequestConnect:
            handle_request_connect(hdr, s, from, now_ms);
            break;
        case PacketType::AcceptConnect:
            handle_accept_connect(hdr, s, now_ms);
            break;
        case PacketType::RejectConnect:
            handle_reject_connect(hdr, s);
            break;
        case PacketType::Disconnect:
            handle_disconnect(hdr, s);
            break;
        case PacketType::DataPacket:
            // Server side: first DataPacket from a client ends AcceptingConnection.
            if (role_ == Role::Server && state_ == State::AcceptingConnection
                && from == peer_)
            {
                state_ = State::Connected;
            }
            // Higher-layer (Track 20/03+) consumes the payload. We don't.
            break;
        case PacketType::Ping:
            // Reply with an empty Ack header.
            {
                VCHeader ack;
                ack.connect_seq_parity = (connect_sequence_ & 1u) != 0;
                ack.send_seq = send_seq_++ & kSendSeqMask;
                ack.highest_recvd_seq = highest_recvd_seq_ & kRecvSeqMask;
                ack.type = PacketType::Ack;
                send_packet(sock_, from, ack, nullptr, 0);
            }
            break;
        case PacketType::Ack:
            // Acks confirm last-sent; reset retry counter.
            retry_count_ = 0;
            break;
        default:
            break;
    }
}

void Connection::handle_request_connect(const VCHeader& hdr, BitStream& s,
                                        const Endpoint& from,
                                        std::uint64_t now_ms)
{
    if (role_ != Role::Server) return;
    if (state_ != State::Unbound) return;  // already serving someone

    auto body_opt = read_request_connect(s);
    if (!body_opt) {
        // Malformed; tell the client.
        peer_ = from;
        connect_sequence_ = hdr.connect_sequence;
        send_reject_connect("Malformed request");
        state_ = State::Failed;
        return;
    }
    const RequestConnectBody& body = *body_opt;

    peer_ = from;
    connect_sequence_ = hdr.connect_sequence;

    if (body.game_id != "TRIBES") {
        send_reject_connect("Wrong game id");
        state_ = State::Failed;
        return;
    }
    if (body.protocol_version != protocol_version_) {
        send_reject_connect("Wrong version");
        state_ = State::Failed;
        return;
    }
    if (!password_.empty() && body.password != password_) {
        send_reject_connect("Wrong password");
        state_ = State::Failed;
        return;
    }
    if (assigned_client_id_ == 0) assigned_client_id_ = 1;
    state_ = State::AcceptingConnection;
    last_sent_ms_ = now_ms;
    send_accept_connect();
}

void Connection::handle_accept_connect(const VCHeader& hdr, BitStream& s,
                                       std::uint64_t now_ms)
{
    if (role_ != Role::Client) return;
    if (state_ != State::RequestingConnection) return;
    if (hdr.connect_sequence != connect_sequence_) return;  // stale

    auto body_opt = read_accept_connect(s);
    if (!body_opt) {
        state_ = State::Failed;
        reject_reason_ = "Malformed AcceptConnect body";
        return;
    }
    assigned_client_id_ = body_opt->assigned_client_id;
    welcome_string_ = body_opt->welcome_string;

    // RTT sample from this round-trip.
    if (last_sent_ms_ != 0) {
        const float sample = static_cast<float>(now_ms - last_sent_ms_);
        rtt_avg_ms_ = (sample + rtt_avg_ms_ * 31.0f) / 32.0f;
    }

    state_ = State::Connected;
    retry_count_ = 0;

    // Spec §4.3: first DataPacket the client sends is the implicit
    // handshake-complete signal. We emit an empty DataPacket so the server
    // can transition out of AcceptingConnection.
    VCHeader ack_hdr;
    ack_hdr.connect_seq_parity = (connect_sequence_ & 1u) != 0;
    ack_hdr.send_seq = send_seq_++ & kSendSeqMask;
    ack_hdr.highest_recvd_seq = highest_recvd_seq_ & kRecvSeqMask;
    ack_hdr.type = PacketType::DataPacket;
    send_packet(sock_, peer_, ack_hdr, nullptr, 0);
    last_sent_ms_ = now_ms;
}

void Connection::handle_reject_connect(const VCHeader& hdr, BitStream& s)
{
    if (role_ != Role::Client) return;
    if (hdr.connect_sequence != connect_sequence_) return;
    auto body_opt = read_reject_connect(s);
    if (body_opt) reject_reason_ = body_opt->reason;
    else reject_reason_ = "(no reason provided)";
    state_ = State::Failed;
}

void Connection::handle_disconnect(const VCHeader& /*hdr*/, BitStream& s)
{
    auto body_opt = read_disconnect(s);
    if (body_opt && !body_opt->reason.empty()) {
        reject_reason_ = body_opt->reason;
    }
    state_ = State::Unbound;
}

} // namespace studio::content::net
