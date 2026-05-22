#ifndef LIB3SPACE_NET_VC_PACKET_HPP
#define LIB3SPACE_NET_VC_PACKET_HPP

// Track 20 spec 02 — VC packet header + connection-control packet bodies.
//
// Source: docs/clean-room-specs/TRIBES-NETPROTO.md §§2-4.
//
// All packets share the VC header (§3.1):
//
//   1   discovery flag (0 for VC traffic; 1 = discovery/master query)
//   1   connect-sequence parity (LSB of connect-sequence nonce, §3.5)
//   9   send sequence (mod 512)
//   5   highest received seq (mod 32)
//   *   ack run-length list (3-bit count; if zero, next 5 bits = packet type)
//   5   packet type (terminates the ack list)
//  32   connect sequence (only for the 4 connection-control packet types)
//   n   payload (only for DataPacket)
//
// Connection-control packet types append the 32-bit connect sequence in
// little-endian byte order after the bit-aligned header.

#include "content/net/bit_stream.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace studio::content::net
{

enum class PacketType : std::uint8_t
{
    DataPacket     = 0,
    Disconnect     = 1,
    RequestConnect = 2,
    AcceptConnect  = 3,
    RejectConnect  = 4,
    Unused         = 6,
    Ping           = 7,
    Resend         = 8,
    Ack            = 16,
};

inline bool is_connection_control(PacketType t)
{
    return t == PacketType::RequestConnect
        || t == PacketType::AcceptConnect
        || t == PacketType::RejectConnect
        || t == PacketType::Disconnect;
}

struct AckRun
{
    std::uint8_t length_minus_one = 0;   // 0..7 (1..8 packets)
    std::uint8_t seq_start = 0;          // 0..31
};

struct VCHeader
{
    bool         discovery_flag = false;            // false for VC traffic
    bool         connect_seq_parity = false;        // LSB of connect sequence
    std::uint16_t send_seq = 0;                     // 0..511
    std::uint8_t  highest_recvd_seq = 0;            // 0..31
    std::vector<AckRun> ack_runs;                   // empty -> immediate terminator
    PacketType   type = PacketType::DataPacket;
    // Only present (and serialised) when type is a connection-control type.
    std::uint32_t connect_sequence = 0;
};

// Write the header into `stream`. For connection-control types, also writes
// the 4-byte little-endian connect_sequence after byte-aligning the bit
// stream. Returns true on success; `stream.is_valid()` may turn false on
// overflow.
bool write_vc_header(BitStream& stream, const VCHeader& h);

// Read the header from `stream`. Returns nullopt if the stream is invalid
// (truncated, unknown packet type, etc.).
std::optional<VCHeader> read_vc_header(BitStream& stream);

// --- Connection-control packet bodies (§§4.2-4.5) ---

struct RequestConnectBody
{
    std::string game_id    = "TRIBES";  // null-not-included on wire
    std::uint16_t protocol_version = 1;  // Tribes 1.41 wire value
    std::string player_name = "OpenSiege";
    std::string password   = "";
};

struct AcceptConnectBody
{
    std::uint32_t assigned_client_id = 0;
    std::string welcome_string = "";
};

struct RejectConnectBody
{
    std::string reason = "";
};

struct DisconnectBody
{
    std::string reason = "";   // optional; empty -> no reason on the wire
};

// Body writers/readers. Each begins right after the VC header
// (including the 4-byte connect_sequence for control packets).
//
// Per the spec §4.2 the protocol version sits at the start of an
// "application-specific blob" written as 2 little-endian bytes. We
// align to byte boundary before writing it.

bool write_request_connect(BitStream& s, const RequestConnectBody& b);
std::optional<RequestConnectBody> read_request_connect(BitStream& s);

bool write_accept_connect(BitStream& s, const AcceptConnectBody& b);
std::optional<AcceptConnectBody> read_accept_connect(BitStream& s);

bool write_reject_connect(BitStream& s, const RejectConnectBody& b);
std::optional<RejectConnectBody> read_reject_connect(BitStream& s);

bool write_disconnect(BitStream& s, const DisconnectBody& b);
std::optional<DisconnectBody> read_disconnect(BitStream& s);

} // namespace studio::content::net

#endif // LIB3SPACE_NET_VC_PACKET_HPP
