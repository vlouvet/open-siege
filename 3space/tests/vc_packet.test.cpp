// Track 20 spec 02 — VC header + connection-control packet body tests.
//
// All vectors taken from docs/clean-room-specs/TRIBES-NETPROTO.md §§3-4.

#include <catch2/catch.hpp>

#include "content/net/bit_stream.hpp"
#include "content/net/vc_packet.hpp"

#include <array>
#include <cstdint>

using namespace studio::content::net;

TEST_CASE("VC header: DataPacket round-trips", "[net][vc_packet]")
{
    std::array<std::uint8_t, 32> buf{};
    VCHeader h;
    h.send_seq = 42;
    h.highest_recvd_seq = 7;
    h.type = PacketType::DataPacket;
    {
        BitStream w(buf.data(), buf.size());
        REQUIRE(write_vc_header(w, h));
    }
    BitStream r(buf.data(), buf.size());
    auto back = read_vc_header(r);
    REQUIRE(back.has_value());
    REQUIRE(back->send_seq == 42);
    REQUIRE(back->highest_recvd_seq == 7);
    REQUIRE(back->type == PacketType::DataPacket);
    REQUIRE(back->ack_runs.empty());
}

TEST_CASE("VC header: RequestConnect carries 32-bit connect sequence LE",
          "[net][vc_packet]")
{
    std::array<std::uint8_t, 32> buf{};
    VCHeader h;
    h.connect_seq_parity = true;
    h.send_seq = 0;
    h.type = PacketType::RequestConnect;
    h.connect_sequence = 0x12345678u;
    {
        BitStream w(buf.data(), buf.size());
        REQUIRE(write_vc_header(w, h));
    }
    BitStream r(buf.data(), buf.size());
    auto back = read_vc_header(r);
    REQUIRE(back.has_value());
    REQUIRE(back->type == PacketType::RequestConnect);
    REQUIRE(back->connect_sequence == 0x12345678u);
    REQUIRE(back->connect_seq_parity == true);
}

TEST_CASE("VC header: ack run list round-trips", "[net][vc_packet]")
{
    std::array<std::uint8_t, 32> buf{};
    VCHeader h;
    h.send_seq = 123;
    h.highest_recvd_seq = 10;
    h.type = PacketType::DataPacket;
    h.ack_runs.push_back({ 2, 4 });   // 3-packet run starting at seq 4
    h.ack_runs.push_back({ 0, 12 });  // 1-packet run at seq 12
    {
        BitStream w(buf.data(), buf.size());
        REQUIRE(write_vc_header(w, h));
    }
    BitStream r(buf.data(), buf.size());
    auto back = read_vc_header(r);
    REQUIRE(back.has_value());
    REQUIRE(back->ack_runs.size() == 2);
    REQUIRE(back->ack_runs[0].length_minus_one == 2);
    REQUIRE(back->ack_runs[0].seq_start == 4);
    REQUIRE(back->ack_runs[1].length_minus_one == 0);
    REQUIRE(back->ack_runs[1].seq_start == 12);
    REQUIRE(back->type == PacketType::DataPacket);
}

TEST_CASE("RequestConnect body: round-trip with version 1",
          "[net][vc_packet]")
{
    std::array<std::uint8_t, 64> buf{};
    RequestConnectBody body;
    body.game_id = "TRIBES";
    body.protocol_version = 1;
    body.player_name = "Bender";
    body.password = "1234";

    {
        BitStream w(buf.data(), buf.size());
        // The body writer assumes the cursor is byte-aligned (post-header).
        // Force alignment by writing the body at offset 0.
        REQUIRE(write_request_connect(w, body));
    }
    BitStream r(buf.data(), buf.size());
    auto back = read_request_connect(r);
    REQUIRE(back.has_value());
    REQUIRE(back->game_id == "TRIBES");
    REQUIRE(back->protocol_version == 1);
    REQUIRE(back->player_name == "Bender");
    REQUIRE(back->password == "1234");
}

TEST_CASE("AcceptConnect body: round-trip", "[net][vc_packet]")
{
    std::array<std::uint8_t, 32> buf{};
    AcceptConnectBody body;
    body.assigned_client_id = 0xCAFEBABEu;
    body.welcome_string = "Welcome to TRIBES";

    {
        BitStream w(buf.data(), buf.size());
        REQUIRE(write_accept_connect(w, body));
    }
    BitStream r(buf.data(), buf.size());
    auto back = read_accept_connect(r);
    REQUIRE(back.has_value());
    REQUIRE(back->assigned_client_id == 0xCAFEBABEu);
    REQUIRE(back->welcome_string == "Welcome to TRIBES");
}

TEST_CASE("RejectConnect body: reason string round-trips", "[net][vc_packet]")
{
    std::array<std::uint8_t, 32> buf{};
    RejectConnectBody body{ "Wrong password" };
    {
        BitStream w(buf.data(), buf.size());
        REQUIRE(write_reject_connect(w, body));
    }
    BitStream r(buf.data(), buf.size());
    auto back = read_reject_connect(r);
    REQUIRE(back.has_value());
    REQUIRE(back->reason == "Wrong password");
}

TEST_CASE("Disconnect body: empty reason is allowed", "[net][vc_packet]")
{
    std::array<std::uint8_t, 32> buf{};
    DisconnectBody body{ "" };
    BitStream w(buf.data(), buf.size());
    REQUIRE(write_disconnect(w, body));
    REQUIRE(w.byte_position() == 0);   // nothing written
}

TEST_CASE("VC header: unknown packet type is rejected", "[net][vc_packet]")
{
    // Manually craft a header with packet type = 5 (not in the allowlist).
    std::array<std::uint8_t, 4> buf{};
    BitStream w(buf.data(), buf.size());
    w.write_flag(false);              // discovery
    w.write_flag(false);              // parity
    w.write_int(0, 9);                // send seq
    w.write_int(0, 5);                // highest recv
    w.write_int(0, 3);                // terminator: count=0
    w.write_int(5, 5);                // unknown type
    REQUIRE(w.is_valid());

    BitStream r(buf.data(), buf.size());
    auto back = read_vc_header(r);
    REQUIRE_FALSE(back.has_value());
}
