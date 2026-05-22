#ifndef LIB3SPACE_NET_BIT_STREAM_HPP
#define LIB3SPACE_NET_BIT_STREAM_HPP

// Track 20 spec 06 — bit-stream primitives.
//
// Source: docs/clean-room-specs/TRIBES-NETPROTO.md §6.
//
// Wire conventions used throughout the Tribes UDP protocol:
//
//   * Bit fields pack MSB-first within each byte. write_flag(true) followed
//     by 7 zeros produces byte 0x80.
//   * Multi-byte byte-aligned fields (rate fields, connect sequence numbers,
//     16-bit discovery keys) are little-endian. The Implementer must align
//     to a byte boundary before reading/writing those.
//   * Strings are byte-aligned with a 1-byte unsigned length prefix and no
//     null terminator on the wire.
//   * Normalized floats: write_float() takes [0, 1] and writes
//     `round(f * (2^bits - 1))` as an unsigned int. write_signed_float()
//     takes [-1, +1] and writes `round(f * (2^(bits-1) - 1))` two's
//     complement.
//
// The BitStream is single-instance read OR write; the same object can be
// used for both directions when constructed against an external byte
// buffer. Bit cursor + validity flag are public-readable for the higher
// layers (event-channel rewind requires get_cur_pos()/set_bit()).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace studio::content::net
{

class BitStream
{
public:
    // Build a stream over an external byte buffer. `capacity_bytes` is the
    // maximum number of bytes the stream may produce or consume; writes that
    // would exceed it leave is_full() = true and set is_valid() = false.
    BitStream(std::uint8_t* buffer, std::size_t capacity_bytes);

    // Convenience: read-only view over a const buffer.
    BitStream(const std::uint8_t* buffer, std::size_t size_bytes);

    // ---- write primitives -------------------------------------------------

    void write_flag(bool b);
    void write_int(std::uint32_t value, int bits);
    void write_signed_int(std::int32_t value, int bits);
    void write_float(float f, int bits);
    void write_signed_float(float f, int bits);

    // Quantized range float: writes the range [min, max] using `bits` bits.
    void write_quantized(float value, float min, float max, int bits);

    // Byte-aligned 1-byte length prefix + raw bytes (max 255 chars).
    void write_string(const std::string& s, std::size_t max_len = 255);

    // Raw byte-aligned write (aligns to next byte boundary first).
    void write_bytes(const void* src, std::size_t n);

    // ---- read primitives --------------------------------------------------

    bool         read_flag();
    std::uint32_t read_int(int bits);
    std::int32_t  read_signed_int(int bits);
    float        read_float(int bits);
    float        read_signed_float(int bits);
    float        read_quantized(float min, float max, int bits);
    std::string  read_string(std::size_t max_len = 255);
    void         read_bytes(void* dst, std::size_t n);

    // ---- bit cursor / rewind support (used by event channel) -------------

    std::size_t  get_cur_pos() const { return bit_pos_; }
    void         set_cur_pos(std::size_t pos) { bit_pos_ = pos; }
    void         set_bit(std::size_t pos, bool val);  // overwrites a single bit

    // Align cursor to next byte boundary, filling with zeros. The stream
    // automatically aligns before strings and raw byte writes.
    void         align_to_byte();

    // ---- state queries ----------------------------------------------------

    bool         is_valid() const { return valid_; }
    bool         is_full()  const { return bit_pos_ > capacity_bits_; }

    std::size_t  bit_position() const { return bit_pos_; }
    std::size_t  byte_position() const { return (bit_pos_ + 7) / 8; }
    std::size_t  capacity_bits() const { return capacity_bits_; }

    // Pointer to underlying buffer (writable variant only).
    std::uint8_t*       data()       { return buffer_; }
    const std::uint8_t* data() const { return buffer_const_; }

private:
    // Internal helpers
    void write_bits_msb(std::uint32_t value, int bits);
    std::uint32_t read_bits_msb(int bits);

    std::uint8_t*       buffer_       = nullptr;  // writable view; nullptr in read-only mode
    const std::uint8_t* buffer_const_ = nullptr;  // always set
    std::size_t bit_pos_     = 0;     // current cursor in bits
    std::size_t capacity_bits_ = 0;   // total capacity in bits
    bool        valid_       = true;
};

// Test-vector helpers exposed for unit tests; not part of the protocol.
namespace detail {

// Rounding convention: round-toward-zero (truncation) on both unsigned and
// signed quantization. The clean-room spec test vectors require this:
//   writeFloat(0.5, 8)        -> 127   (= trunc(127.5))
//   writeSignedFloat(-0.5, 8) -> -63   (= trunc(-63.5) toward zero)
// We document this as the deterministic rule for the project so behaviour
// is reproducible across builds and tooling.

inline std::uint32_t quantize_unsigned(float f, int bits)
{
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    const std::uint32_t scale = (bits >= 32) ? 0xFFFFFFFFu
                                             : ((1u << bits) - 1u);
    return static_cast<std::uint32_t>(f * static_cast<float>(scale));
}

inline std::int32_t quantize_signed(float f, int bits)
{
    if (f < -1.0f) f = -1.0f;
    if (f >  1.0f) f =  1.0f;
    const std::int32_t scale = (bits >= 32) ? 0x7FFFFFFF
                                            : ((1 << (bits - 1)) - 1);
    return static_cast<std::int32_t>(f * static_cast<float>(scale));
}

} // namespace detail

} // namespace studio::content::net

#endif // LIB3SPACE_NET_BIT_STREAM_HPP
