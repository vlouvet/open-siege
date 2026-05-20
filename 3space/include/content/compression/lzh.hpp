#ifndef DARKSTAR_COMPRESSION_LZH_HPP
#define DARKSTAR_COMPRESSION_LZH_HPP

#include <cstddef>
#include <cstdint>
#include <istream>
#include <vector>

// Clean-room implementation of the LZH stream codec used inside the
// Dynamix Darkstar 3space engine. The codec is bit-for-bit equivalent to
// LHarc's "-lh1-" method (LZSS over a 4 KiB sliding window with an
// adaptive Huffman tree over 314 symbols; static 64-class prefix table
// for back-reference offsets).
//
// Implemented from the spec at docs/clean-room-specs/LZH-CODEC.md and
// cross-checked against the public clean-room reference liblhasa
// (Simon Howard, ISC-licensed). No Dynamix engine source was consulted
// during implementation.
//
// Stream framing: the codec itself carries no header or terminator.
// The caller supplies the expected uncompressed byte count externally
// (in every observed Dynamix container, a little-endian u32 immediately
// precedes the raw bit-stream). The decoder reads bits MSB-first within
// each byte and stops as soon as `expected_output_size` bytes have been
// emitted, even mid back-reference run.

namespace studio::content::compression
{
  // Decode an LZH (-lh1-) bit-stream from `in` until exactly
  // `expected_output_size` bytes have been produced.
  //
  // Throws std::runtime_error if `in` is exhausted before the expected
  // count is reached, or if the bit-stream is internally inconsistent
  // (Huffman state corruption).
  std::vector<std::byte> lzh_decompress(
    std::istream& in,
    std::size_t expected_output_size);

  // Convenience wrapper around the in-memory case: takes a raw byte
  // span and decodes a known number of output bytes from it.
  std::vector<std::byte> lzh_decompress(
    const std::byte* data,
    std::size_t size,
    std::size_t expected_output_size);
}

#endif
