#include "content/net/bit_stream.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace studio::content::net
{

BitStream::BitStream(std::uint8_t* buffer, std::size_t capacity_bytes)
    : buffer_(buffer),
      buffer_const_(buffer),
      bit_pos_(0),
      capacity_bits_(capacity_bytes * 8),
      valid_(true)
{
    // Intentionally do NOT zero the buffer. Constructing a BitStream against
    // an already-filled buffer (e.g. read-back after write) must not destroy
    // its contents. The write primitives clear-then-set each bit explicitly.
}

BitStream::BitStream(const std::uint8_t* buffer, std::size_t size_bytes)
    : buffer_(nullptr),
      buffer_const_(buffer),
      bit_pos_(0),
      capacity_bits_(size_bytes * 8),
      valid_(true)
{
}

void BitStream::write_bits_msb(std::uint32_t value, int bits)
{
    if (!buffer_) { valid_ = false; return; }
    if (bits <= 0 || bits > 32) { valid_ = false; return; }
    if (bit_pos_ + static_cast<std::size_t>(bits) > capacity_bits_) {
        valid_ = false;
        bit_pos_ += bits;  // keep accumulating so is_full() trips correctly
        return;
    }

    // MSB-first within each byte. For each output bit (high-to-low of `value`
    // in the bits..0 range), set the corresponding stream bit. The stream's
    // first bit lives at byte[0] >> 7.
    for (int i = bits - 1; i >= 0; --i) {
        const std::uint32_t bit_v = (value >> i) & 1u;
        const std::size_t byte_idx = bit_pos_ / 8;
        const std::size_t bit_in_byte = 7 - (bit_pos_ % 8);  // MSB-first
        if (bit_v) {
            buffer_[byte_idx] |= static_cast<std::uint8_t>(1u << bit_in_byte);
        } else {
            // Pre-cleared buffer means we don't need to mask; but if we're
            // writing into a buffer that was reused (set_bit), we should
            // clear stale bits. Cheap to always do.
            buffer_[byte_idx] &= static_cast<std::uint8_t>(
                ~(1u << bit_in_byte));
        }
        ++bit_pos_;
    }
}

std::uint32_t BitStream::read_bits_msb(int bits)
{
    if (bits <= 0 || bits > 32) { valid_ = false; return 0; }
    if (bit_pos_ + static_cast<std::size_t>(bits) > capacity_bits_) {
        valid_ = false;
        return 0;
    }
    std::uint32_t out = 0;
    for (int i = 0; i < bits; ++i) {
        const std::size_t byte_idx = bit_pos_ / 8;
        const std::size_t bit_in_byte = 7 - (bit_pos_ % 8);
        const std::uint32_t bit_v =
            (buffer_const_[byte_idx] >> bit_in_byte) & 1u;
        out = (out << 1) | bit_v;
        ++bit_pos_;
    }
    return out;
}

void BitStream::write_flag(bool b)
{
    write_bits_msb(b ? 1u : 0u, 1);
}

bool BitStream::read_flag()
{
    return read_bits_msb(1) != 0;
}

void BitStream::write_int(std::uint32_t value, int bits)
{
    if (bits < 32) {
        const std::uint32_t mask = (1u << bits) - 1u;
        value &= mask;
    }
    write_bits_msb(value, bits);
}

std::uint32_t BitStream::read_int(int bits)
{
    return read_bits_msb(bits);
}

void BitStream::write_signed_int(std::int32_t value, int bits)
{
    if (bits <= 0 || bits > 32) { valid_ = false; return; }
    // Sign-extend / mask down to `bits` width, then write as unsigned.
    const std::uint32_t mask = (bits >= 32) ? 0xFFFFFFFFu
                                            : ((1u << bits) - 1u);
    const std::uint32_t u = static_cast<std::uint32_t>(value) & mask;
    write_bits_msb(u, bits);
}

std::int32_t BitStream::read_signed_int(int bits)
{
    if (bits <= 0 || bits > 32) { valid_ = false; return 0; }
    const std::uint32_t u = read_bits_msb(bits);
    // Sign-extend
    if (bits == 32) return static_cast<std::int32_t>(u);
    const std::uint32_t sign_bit = 1u << (bits - 1);
    if (u & sign_bit) {
        const std::uint32_t extended = u | (~((1u << bits) - 1u));
        return static_cast<std::int32_t>(extended);
    }
    return static_cast<std::int32_t>(u);
}

void BitStream::write_float(float f, int bits)
{
    write_int(detail::quantize_unsigned(f, bits), bits);
}

float BitStream::read_float(int bits)
{
    const std::uint32_t u = read_int(bits);
    const std::uint32_t scale = (bits >= 32) ? 0xFFFFFFFFu
                                             : ((1u << bits) - 1u);
    if (scale == 0) return 0.0f;
    return static_cast<float>(u) / static_cast<float>(scale);
}

void BitStream::write_signed_float(float f, int bits)
{
    write_signed_int(detail::quantize_signed(f, bits), bits);
}

float BitStream::read_signed_float(int bits)
{
    const std::int32_t v = read_signed_int(bits);
    const std::int32_t scale = (bits >= 32) ? 0x7FFFFFFF
                                            : ((1 << (bits - 1)) - 1);
    if (scale == 0) return 0.0f;
    return static_cast<float>(v) / static_cast<float>(scale);
}

void BitStream::write_quantized(float value, float min, float max, int bits)
{
    const float range = max - min;
    const float norm = (range > 0.0f) ? ((value - min) / range) : 0.0f;
    write_float(norm, bits);
}

float BitStream::read_quantized(float min, float max, int bits)
{
    const float norm = read_float(bits);
    return min + norm * (max - min);
}

void BitStream::align_to_byte()
{
    const std::size_t rem = bit_pos_ % 8;
    if (rem == 0) return;
    const std::size_t pad = 8 - rem;
    // Pad with zeros (advance cursor; buffer is already zeroed on construct).
    if (bit_pos_ + pad > capacity_bits_) {
        valid_ = false;
        bit_pos_ = capacity_bits_;
        return;
    }
    bit_pos_ += pad;
}

void BitStream::write_string(const std::string& s, std::size_t max_len)
{
    align_to_byte();
    const std::size_t len = std::min(s.size(), std::min<std::size_t>(max_len, 255));
    if (!buffer_) { valid_ = false; return; }
    if (bit_pos_ + (1 + len) * 8 > capacity_bits_) {
        valid_ = false;
        bit_pos_ += (1 + len) * 8;
        return;
    }
    buffer_[bit_pos_ / 8] = static_cast<std::uint8_t>(len);
    bit_pos_ += 8;
    if (len > 0) {
        std::memcpy(buffer_ + bit_pos_ / 8, s.data(), len);
        bit_pos_ += len * 8;
    }
}

std::string BitStream::read_string(std::size_t max_len)
{
    align_to_byte();
    if (bit_pos_ + 8 > capacity_bits_) { valid_ = false; return {}; }
    const std::size_t len = buffer_const_[bit_pos_ / 8];
    bit_pos_ += 8;
    if (bit_pos_ + len * 8 > capacity_bits_) { valid_ = false; return {}; }
    if (len > max_len) {
        // Server-supplied length exceeded our buffer; bail rather than
        // overrun.
        valid_ = false;
        return {};
    }
    std::string out;
    out.resize(len);
    if (len > 0) {
        std::memcpy(out.data(), buffer_const_ + bit_pos_ / 8, len);
        bit_pos_ += len * 8;
    }
    return out;
}

void BitStream::write_bytes(const void* src, std::size_t n)
{
    align_to_byte();
    if (!buffer_) { valid_ = false; return; }
    if (bit_pos_ + n * 8 > capacity_bits_) {
        valid_ = false;
        bit_pos_ += n * 8;
        return;
    }
    std::memcpy(buffer_ + bit_pos_ / 8, src, n);
    bit_pos_ += n * 8;
}

void BitStream::read_bytes(void* dst, std::size_t n)
{
    align_to_byte();
    if (bit_pos_ + n * 8 > capacity_bits_) { valid_ = false; return; }
    std::memcpy(dst, buffer_const_ + bit_pos_ / 8, n);
    bit_pos_ += n * 8;
}

void BitStream::set_bit(std::size_t pos, bool val)
{
    if (!buffer_) { valid_ = false; return; }
    if (pos >= capacity_bits_) { valid_ = false; return; }
    const std::size_t byte_idx = pos / 8;
    const std::size_t bit_in_byte = 7 - (pos % 8);
    if (val) {
        buffer_[byte_idx] |= static_cast<std::uint8_t>(1u << bit_in_byte);
    } else {
        buffer_[byte_idx] &= static_cast<std::uint8_t>(~(1u << bit_in_byte));
    }
}

} // namespace studio::content::net
