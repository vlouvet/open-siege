#include "content/net/vc_packet.hpp"

#include <cstring>

namespace studio::content::net
{

namespace
{
// Allowlist of packet types we accept on the wire. Reject everything else
// per spec §3.2 (treat the "Unused" tombstone as a parse failure).
bool is_valid_packet_type(std::uint8_t v)
{
    switch (static_cast<PacketType>(v)) {
        case PacketType::DataPacket:
        case PacketType::Disconnect:
        case PacketType::RequestConnect:
        case PacketType::AcceptConnect:
        case PacketType::RejectConnect:
        case PacketType::Ping:
        case PacketType::Resend:
        case PacketType::Ack:
            return true;
        default:
            return false;
    }
}

// Little-endian 32-bit byte write/read. The stream auto-aligns before
// write_bytes(), so the underlying memcpy is byte-aligned.
void write_u32_le(BitStream& s, std::uint32_t v)
{
    std::uint8_t bytes[4] = {
        static_cast<std::uint8_t>(v & 0xFF),
        static_cast<std::uint8_t>((v >> 8) & 0xFF),
        static_cast<std::uint8_t>((v >> 16) & 0xFF),
        static_cast<std::uint8_t>((v >> 24) & 0xFF),
    };
    s.write_bytes(bytes, 4);
}

std::uint32_t read_u32_le(BitStream& s)
{
    std::uint8_t bytes[4]{};
    s.read_bytes(bytes, 4);
    return  static_cast<std::uint32_t>(bytes[0])
         | (static_cast<std::uint32_t>(bytes[1]) << 8)
         | (static_cast<std::uint32_t>(bytes[2]) << 16)
         | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

void write_u16_le(BitStream& s, std::uint16_t v)
{
    std::uint8_t bytes[2] = {
        static_cast<std::uint8_t>(v & 0xFF),
        static_cast<std::uint8_t>((v >> 8) & 0xFF),
    };
    s.write_bytes(bytes, 2);
}

std::uint16_t read_u16_le(BitStream& s)
{
    std::uint8_t bytes[2]{};
    s.read_bytes(bytes, 2);
    return static_cast<std::uint16_t>(bytes[0])
        | (static_cast<std::uint16_t>(bytes[1]) << 8);
}

} // anonymous namespace

bool write_vc_header(BitStream& s, const VCHeader& h)
{
    s.write_flag(h.discovery_flag);
    s.write_flag(h.connect_seq_parity);
    s.write_int(h.send_seq & 0x1FFu, 9);
    s.write_int(h.highest_recvd_seq & 0x1Fu, 5);

    // Ack runs: each is (3-bit count-1, 5-bit start). Terminator is
    // 3-bit zero followed by 5-bit packet type.
    for (const auto& run : h.ack_runs) {
        if (run.length_minus_one > 7) { return false; }   // run too long
        s.write_int(static_cast<std::uint32_t>(run.length_minus_one) + 1u, 3);
        s.write_int(run.seq_start & 0x1Fu, 5);
    }
    // Terminator: count=0 followed by 5-bit packet type.
    s.write_int(0u, 3);
    s.write_int(static_cast<std::uint32_t>(h.type) & 0x1Fu, 5);

    if (is_connection_control(h.type)) {
        write_u32_le(s, h.connect_sequence);
    }
    return s.is_valid();
}

std::optional<VCHeader> read_vc_header(BitStream& s)
{
    VCHeader h;
    h.discovery_flag = s.read_flag();
    h.connect_seq_parity = s.read_flag();
    h.send_seq = static_cast<std::uint16_t>(s.read_int(9));
    h.highest_recvd_seq = static_cast<std::uint8_t>(s.read_int(5));

    // Walk ack runs until terminator (count == 0).
    for (;;) {
        if (!s.is_valid()) return std::nullopt;
        const std::uint32_t count = s.read_int(3);
        if (count == 0) {
            const std::uint32_t type_v = s.read_int(5);
            if (!is_valid_packet_type(static_cast<std::uint8_t>(type_v))) {
                return std::nullopt;
            }
            h.type = static_cast<PacketType>(type_v);
            break;
        }
        AckRun run;
        run.length_minus_one = static_cast<std::uint8_t>(count - 1);
        run.seq_start = static_cast<std::uint8_t>(s.read_int(5));
        h.ack_runs.push_back(run);
        if (h.ack_runs.size() > 64) return std::nullopt;  // sanity cap
    }

    if (is_connection_control(h.type)) {
        h.connect_sequence = read_u32_le(s);
    }
    return s.is_valid() ? std::optional<VCHeader>(h) : std::nullopt;
}

// --- Connection-control bodies ---

bool write_request_connect(BitStream& s, const RequestConnectBody& b)
{
    // BitStream auto-aligns to byte for strings + raw bytes, so the field
    // order written here matches the spec byte layout.
    s.write_string(b.game_id);
    write_u16_le(s, b.protocol_version);
    s.write_string(b.player_name, 16);
    s.write_string(b.password);
    return s.is_valid();
}

std::optional<RequestConnectBody> read_request_connect(BitStream& s)
{
    RequestConnectBody b;
    b.game_id = s.read_string();
    b.protocol_version = read_u16_le(s);
    b.player_name = s.read_string(16);
    b.password = s.read_string();
    if (!s.is_valid()) return std::nullopt;
    return b;
}

bool write_accept_connect(BitStream& s, const AcceptConnectBody& b)
{
    write_u32_le(s, b.assigned_client_id);
    s.write_string(b.welcome_string);
    return s.is_valid();
}

std::optional<AcceptConnectBody> read_accept_connect(BitStream& s)
{
    AcceptConnectBody b;
    b.assigned_client_id = read_u32_le(s);
    b.welcome_string = s.read_string();
    if (!s.is_valid()) return std::nullopt;
    return b;
}

bool write_reject_connect(BitStream& s, const RejectConnectBody& b)
{
    s.write_string(b.reason);
    return s.is_valid();
}

std::optional<RejectConnectBody> read_reject_connect(BitStream& s)
{
    RejectConnectBody b;
    b.reason = s.read_string();
    if (!s.is_valid()) return std::nullopt;
    return b;
}

bool write_disconnect(BitStream& s, const DisconnectBody& b)
{
    if (!b.reason.empty()) s.write_string(b.reason);
    return s.is_valid();
}

std::optional<DisconnectBody> read_disconnect(BitStream& s)
{
    DisconnectBody b;
    // Reason string is optional — if no bytes remain past the header, treat
    // it as empty.
    if (s.bit_position() + 8 <= s.capacity_bits()) {
        b.reason = s.read_string();
    }
    if (!s.is_valid()) return std::nullopt;
    return b;
}

} // namespace studio::content::net
