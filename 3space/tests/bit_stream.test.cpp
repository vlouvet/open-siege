// Track 20 spec 06 — BitStream unit tests.
//
// All vectors taken from docs/clean-room-specs/TRIBES-NETPROTO.md §6 and
// the spec's test-vector table. No leaked source consulted.

#include <catch2/catch.hpp>

#include "content/net/bit_stream.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace studio::content::net;

TEST_CASE("BitStream: write_flag is MSB-first within a byte", "[net][bit_stream]")
{
    std::array<std::uint8_t, 1> buf{};
    BitStream w(buf.data(), buf.size());
    w.write_flag(true);
    for (int i = 0; i < 7; ++i) w.write_flag(false);
    REQUIRE(buf[0] == 0x80);
    REQUIRE(w.bit_position() == 8);
    REQUIRE(w.is_valid());

    BitStream r(buf.data(), buf.size());
    REQUIRE(r.read_flag() == true);
    for (int i = 0; i < 7; ++i) REQUIRE(r.read_flag() == false);
}

TEST_CASE("BitStream: write_int round-trips across byte boundary", "[net][bit_stream]")
{
    std::array<std::uint8_t, 4> buf{};
    BitStream w(buf.data(), buf.size());
    w.write_int(0b1111u, 4);
    w.write_int(0xABu, 8);     // crosses bit-4 -> bit-12 boundary
    w.write_int(0x1234u, 16);
    REQUIRE(w.is_valid());

    BitStream r(buf.data(), buf.size());
    REQUIRE(r.read_int(4)  == 0xFu);
    REQUIRE(r.read_int(8)  == 0xABu);
    REQUIRE(r.read_int(16) == 0x1234u);
}

TEST_CASE("BitStream: signed int sign-extends correctly", "[net][bit_stream]")
{
    std::array<std::uint8_t, 4> buf{};
    BitStream w(buf.data(), buf.size());
    w.write_signed_int(-1, 8);     // 0xFF
    w.write_signed_int(-64, 7);    // 7-bit two's complement, sign-extends to -64
    w.write_signed_int(63, 7);
    REQUIRE(w.is_valid());

    BitStream r(buf.data(), buf.size());
    REQUIRE(r.read_signed_int(8) == -1);
    REQUIRE(r.read_signed_int(7) == -64);
    REQUIRE(r.read_signed_int(7) == 63);
}

TEST_CASE("BitStream: write_float(0.5, 8) wire = 0x7F", "[net][bit_stream]")
{
    // Spec §6.4 test vector: writeFloat(0.5, 8) -> wire bits 01111111 (127).
    std::array<std::uint8_t, 1> buf{};
    BitStream w(buf.data(), buf.size());
    w.write_float(0.5f, 8);
    REQUIRE(buf[0] == 0x7F);  // 127 left-aligned in the byte

    BitStream r(buf.data(), buf.size());
    const float back = r.read_float(8);
    REQUIRE(back == Approx(127.0f / 255.0f).margin(1e-6f));
}

TEST_CASE("BitStream: signed float round-trip", "[net][bit_stream]")
{
    // Per the spec: write_signed_float clamps to [-1, +1] and writes
    // round(f * (2^(bits-1) - 1)) two's-complement. For -0.5 at 8 bits,
    // round(-0.5 * 127 + 0.5) = -63 — we pick round-half-toward-zero so
    // values stay deterministic across builds.
    std::array<std::uint8_t, 2> buf{};
    BitStream w(buf.data(), buf.size());
    w.write_signed_float(-0.5f, 8);
    w.write_signed_float( 1.0f, 8);

    BitStream r(buf.data(), buf.size());
    REQUIRE(r.read_signed_float(8) == Approx(-63.0f / 127.0f).margin(1.5e-2f));
    REQUIRE(r.read_signed_float(8) == Approx( 1.0f).margin(1e-6f));
}

TEST_CASE("BitStream: write_quantized covers position-style ranges", "[net][bit_stream]")
{
    // Player position X spec: range -2048..+2048, 16 bits.
    std::array<std::uint8_t, 4> buf{};
    BitStream w(buf.data(), buf.size());
    w.write_quantized(-2048.0f, -2048.0f, 2048.0f, 16);
    w.write_quantized(    0.0f, -2048.0f, 2048.0f, 16);

    BitStream r(buf.data(), buf.size());
    REQUIRE(r.read_quantized(-2048.0f, 2048.0f, 16)
            == Approx(-2048.0f).margin(0.1f));
    REQUIRE(r.read_quantized(-2048.0f, 2048.0f, 16)
            == Approx(0.0f).margin(0.1f));
}

TEST_CASE("BitStream: write_string is byte-aligned + length-prefixed",
          "[net][bit_stream]")
{
    std::array<std::uint8_t, 32> buf{};
    BitStream w(buf.data(), buf.size());
    // Write a single bit first so the string write must align to byte 1.
    w.write_flag(true);
    w.write_string("TRIBES");
    REQUIRE(w.is_valid());
    REQUIRE(buf[0] == 0x80);          // the single bit
    REQUIRE(buf[1] == 6);             // length prefix
    REQUIRE(buf[2] == 'T');
    REQUIRE(buf[3] == 'R');
    REQUIRE(buf[4] == 'I');
    REQUIRE(buf[5] == 'B');
    REQUIRE(buf[6] == 'E');
    REQUIRE(buf[7] == 'S');

    BitStream r(buf.data(), buf.size());
    REQUIRE(r.read_flag() == true);
    REQUIRE(r.read_string() == "TRIBES");
}

TEST_CASE("BitStream: write_bytes auto-aligns to byte boundary", "[net][bit_stream]")
{
    std::array<std::uint8_t, 16> buf{};
    BitStream w(buf.data(), buf.size());
    w.write_int(0b101u, 3);           // leaves cursor at bit 3
    std::uint32_t raw_le = 0xDEADBEEFu;  // LE on the wire
    w.write_bytes(&raw_le, 4);
    // After auto-align the cursor jumps from bit 3 to bit 8.
    REQUIRE(w.byte_position() == 5);   // 1 (aligned 3-bit field) + 4 raw
    REQUIRE(buf[1] == 0xEF);
    REQUIRE(buf[2] == 0xBE);
    REQUIRE(buf[3] == 0xAD);
    REQUIRE(buf[4] == 0xDE);

    BitStream r(buf.data(), buf.size());
    REQUIRE(r.read_int(3) == 0b101u);
    std::uint32_t back = 0;
    r.read_bytes(&back, 4);
    REQUIRE(back == 0xDEADBEEFu);
}

TEST_CASE("BitStream: set_bit overwrites a single bit at an earlier offset",
          "[net][bit_stream]")
{
    // Event-channel rewind pattern: write an event-present flag at offset N,
    // then on overflow rewind cursor + clear the flag with set_bit(N, false).
    std::array<std::uint8_t, 4> buf{};
    BitStream w(buf.data(), buf.size());
    const std::size_t mark = w.get_cur_pos();
    w.write_flag(true);
    w.write_int(0xFFu, 8);
    // Pretend overflow: rewind to mark + clear the present flag.
    w.set_cur_pos(mark);
    w.set_bit(mark, false);
    REQUIRE((buf[0] & 0x80) == 0);
}

TEST_CASE("BitStream: writes past capacity flip is_valid()", "[net][bit_stream]")
{
    std::array<std::uint8_t, 2> buf{};
    BitStream w(buf.data(), buf.size());
    // Capacity is 16 bits. Write 8, 8, then one more should overflow.
    w.write_int(0xAAu, 8);
    w.write_int(0xBBu, 8);
    REQUIRE(w.is_valid());
    REQUIRE_FALSE(w.is_full());
    w.write_flag(true);
    REQUIRE_FALSE(w.is_valid());
    REQUIRE(w.is_full());
}

TEST_CASE("BitStream: 10K small-record pack/unpack budget < 10ms",
          "[net][bit_stream][.perf]")
{
    // Spec acceptance: pack/unpack 10K small records < 10ms.
    // We use 8-byte records: header + 3 quantized floats. Tagged `.perf` so
    // it can be skipped on slow CI agents via `--exclude "[.perf]"`.
    constexpr int kRecords = 10000;
    std::vector<std::uint8_t> buf(kRecords * 8);

    auto t0 = std::chrono::steady_clock::now();
    {
        BitStream w(buf.data(), buf.size());
        for (int i = 0; i < kRecords; ++i) {
            w.write_int(static_cast<std::uint32_t>(i & 0x3FF), 10);
            w.write_quantized(float((i % 1000) - 500), -2048.0f, 2048.0f, 16);
            w.write_quantized(float(i % 1000), 0.0f, 2048.0f, 16);
            w.write_signed_float(0.25f, 8);
            w.write_flag(true);
            w.align_to_byte();
        }
    }
    {
        BitStream r(buf.data(), buf.size());
        for (int i = 0; i < kRecords; ++i) {
            (void)r.read_int(10);
            (void)r.read_quantized(-2048.0f, 2048.0f, 16);
            (void)r.read_quantized(0.0f, 2048.0f, 16);
            (void)r.read_signed_float(8);
            (void)r.read_flag();
            r.align_to_byte();
        }
    }
    auto dt = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
    INFO("10K small-record round-trip took " << dt << " us");
    REQUIRE(dt < 10'000);
}
