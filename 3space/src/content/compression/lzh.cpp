// Clean-room LZH ("-lh1-") decoder for the Dynamix Darkstar 3space
// engine. Built from docs/clean-room-specs/LZH-CODEC.md, cross-checked
// against the public clean-room reference liblhasa (Simon Howard,
// ISC-licensed) for the LH1 algorithm. No Dynamix source was consulted.
//
// Algorithm summary:
//   - 4096-byte sliding window prefilled with ASCII space (0x20),
//     initial write cursor at 4036.
//   - Adaptive Faller/Gallager/Knuth/Vitter Huffman tree over 314
//     symbols (0..255 literals, 256..313 copy-length 3..60).
//   - Back-reference offsets are coded as an 8-bit prefix consulting two
//     fixed 256-entry tables (d_code, d_len) giving the top 6 bits and
//     the actual prefix length 3..8, followed by 6 verbatim bits.
//   - Tree renormalises (halve all frequencies, rebuild) when the root
//     frequency reaches 0x8000.

#include "content/compression/lzh.hpp"

#include <array>
#include <cstdint>
#include <istream>
#include <sstream>
#include <stdexcept>

namespace studio::content::compression
{
  namespace
  {
    // --- fixed codec parameters (see spec section 3) -------------------
    constexpr std::size_t window_size = 4096;
    constexpr std::size_t lookahead = 60;
    constexpr std::size_t copy_threshold = 3;
    constexpr std::size_t window_initial_cursor = window_size - lookahead;  // 4036
    constexpr std::uint8_t window_fill_byte = 0x20;                          // ASCII space

    constexpr std::size_t literal_alphabet = 256;
    constexpr std::size_t length_alphabet = 58;     // codes 256..313 -> lengths 3..60
    constexpr std::size_t total_symbols = literal_alphabet + length_alphabet;  // 314
    constexpr std::size_t tree_node_count = total_symbols * 2 - 1;             // 627
    constexpr std::size_t root_slot = tree_node_count - 1;                     // 626
    constexpr std::uint16_t freq_rebuild_threshold = 0x8000;
    constexpr std::uint16_t freq_sentinel = 0xFFFF;
    constexpr std::size_t leaf_offset = tree_node_count;                       // 627

    constexpr std::size_t position_classes = 64;
    constexpr std::size_t position_suffix_bits = 6;

    // --- static prefix tables for back-reference offsets (spec 5.1) ---
    constexpr std::array<std::uint8_t, 256> d_code = {
      0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
      1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2,
      3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, 4,4,4,4,4,4,4,4, 5,5,5,5,5,5,5,5,
      6,6,6,6,6,6,6,6, 7,7,7,7,7,7,7,7, 8,8,8,8,8,8,8,8, 9,9,9,9,9,9,9,9,
      10,10,10,10,10,10,10,10, 11,11,11,11,11,11,11,11,
      12,12,12,12, 13,13,13,13, 14,14,14,14, 15,15,15,15,
      16,16,16,16, 17,17,17,17, 18,18,18,18, 19,19,19,19,
      20,20,20,20, 21,21,21,21, 22,22,22,22, 23,23,23,23,
      24,24, 25,25, 26,26, 27,27, 28,28, 29,29, 30,30, 31,31,
      32,32, 33,33, 34,34, 35,35, 36,36, 37,37, 38,38, 39,39,
      40,40, 41,41, 42,42, 43,43, 44,44, 45,45, 46,46, 47,47,
      48,49,50,51,52,53,54,55, 56,57,58,59,60,61,62,63
    };

    constexpr std::array<std::uint8_t, 256> d_len = {
      3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,
      4,4,4,4,4,4,4,4, 4,4,4,4,4,4,4,4, 4,4,4,4,4,4,4,4, 4,4,4,4,4,4,4,4,
      4,4,4,4,4,4,4,4, 4,4,4,4,4,4,4,4,
      5,5,5,5,5,5,5,5, 5,5,5,5,5,5,5,5, 5,5,5,5,5,5,5,5, 5,5,5,5,5,5,5,5,
      5,5,5,5,5,5,5,5, 5,5,5,5,5,5,5,5, 5,5,5,5,5,5,5,5, 5,5,5,5,5,5,5,5,
      6,6,6,6,6,6,6,6, 6,6,6,6,6,6,6,6, 6,6,6,6,6,6,6,6, 6,6,6,6,6,6,6,6,
      6,6,6,6,6,6,6,6, 6,6,6,6,6,6,6,6,
      7,7,7,7,7,7,7,7, 7,7,7,7,7,7,7,7, 7,7,7,7,7,7,7,7, 7,7,7,7,7,7,7,7,
      7,7,7,7,7,7,7,7, 7,7,7,7,7,7,7,7,
      8,8,8,8,8,8,8,8, 8,8,8,8,8,8,8,8
    };

    // --- MSB-first bit reader over an std::istream --------------------
    class bit_reader
    {
    public:
      explicit bit_reader(std::istream& in) : in_(in) {}

      // Consume next `bit_count` bits (1..16), MSB-first within each
      // byte. The first bit returned is bit 7 of the first byte ever
      // read, the next bit is bit 6 of the same byte, and so on.
      std::uint16_t read_bits(unsigned bit_count)
      {
        while (buffered_ < bit_count)
        {
          int ch = in_.get();
          // Per spec section 6, the engine zero-pads on EOF rather than
          // signalling — match that for terrain-style truncated streams.
          std::uint8_t byte = (ch == EOF) ? 0 : static_cast<std::uint8_t>(ch);
          buffer_ = static_cast<std::uint32_t>(
            (buffer_ << 8) | byte);
          buffered_ += 8;
        }
        unsigned shift = buffered_ - bit_count;
        std::uint16_t out = static_cast<std::uint16_t>(
          (buffer_ >> shift) & ((1u << bit_count) - 1u));
        buffered_ -= bit_count;
        buffer_ &= (1u << buffered_) - 1u;
        return out;
      }

      // Peek (without consuming) the next 8 bits MSB-first.
      std::uint8_t peek_byte()
      {
        while (buffered_ < 8)
        {
          int ch = in_.get();
          std::uint8_t byte = (ch == EOF) ? 0 : static_cast<std::uint8_t>(ch);
          buffer_ = static_cast<std::uint32_t>((buffer_ << 8) | byte);
          buffered_ += 8;
        }
        return static_cast<std::uint8_t>((buffer_ >> (buffered_ - 8)) & 0xFFu);
      }

      // Consume exactly `bit_count` bits previously observed via
      // peek_byte (no return value).
      void consume_bits(unsigned bit_count)
      {
        // Guaranteed by prior peek to have at least 8 buffered.
        buffered_ -= bit_count;
        buffer_ &= (1u << buffered_) - 1u;
      }

    private:
      std::istream& in_;
      std::uint32_t buffer_ = 0;
      unsigned buffered_ = 0;
    };

    // --- adaptive Huffman state (spec section 4) ----------------------
    struct huffman_tree
    {
      // freq has one extra sentinel slot at index tree_node_count
      // (627) holding 0xFFFF so the forward swap-scan terminates.
      std::array<std::uint16_t, tree_node_count + 1> freq{};

      // son[i] = left-child slot for internal i; for a leaf,
      // son[i] >= tree_node_count and the represented symbol is
      // son[i] - tree_node_count.
      std::array<std::uint16_t, tree_node_count> son{};

      // parent[i] for i < tree_node_count is the parent slot. Indices
      // tree_node_count .. tree_node_count + total_symbols - 1 form a
      // reverse map: parent[tree_node_count + s] is the slot index
      // currently holding the leaf for symbol s.
      std::array<std::uint16_t, tree_node_count + total_symbols> parent{};

      void init()
      {
        // Leaves: one slot per symbol, frequency 1.
        for (std::size_t i = 0; i < total_symbols; ++i)
        {
          freq[i] = 1;
          son[i] = static_cast<std::uint16_t>(leaf_offset + i);
          parent[leaf_offset + i] = static_cast<std::uint16_t>(i);
        }
        // Internal nodes built bottom-up: pair (i, i+1) -> i+N_CHAR/2..
        // The leaves all have weight 1, so the initial tree is balanced
        // and the pairing rule reduces to "leaf 2k, 2k+1 -> internal
        // slot total_symbols + k".
        std::size_t i = 0;
        std::size_t j = total_symbols;
        while (j < tree_node_count)
        {
          freq[j] = static_cast<std::uint16_t>(freq[i] + freq[i + 1]);
          son[j] = static_cast<std::uint16_t>(i);
          parent[i] = parent[i + 1] = static_cast<std::uint16_t>(j);
          i += 2;
          ++j;
        }
        freq[tree_node_count] = freq_sentinel;
        parent[root_slot] = 0;
      }

      // Frequency normalisation + rebuild — spec section 4.4.
      void rebuild()
      {
        // Gather the live leaves with halved (round-up) frequencies.
        // The current tree's leaves occupy 314 of the 627 slots, but
        // not necessarily slots 0..313 — they have been shuffled by
        // node-swap during decoding. Walk all slots and copy out the
        // leaves (where son[i] >= leaf_offset).
        std::size_t j = 0;
        for (std::size_t i = 0; i < tree_node_count; ++i)
        {
          if (son[i] >= leaf_offset)
          {
            freq[j] = static_cast<std::uint16_t>((freq[i] + 1) >> 1);
            son[j] = son[i];
            ++j;
          }
        }
        // Sanity (cannot happen for a well-formed tree): j must equal
        // total_symbols.
        if (j != total_symbols)
        {
          throw std::runtime_error("lzh: corrupt Huffman tree during rebuild");
        }

        // Rebuild internal nodes. For each new internal slot k in
        // [total_symbols, tree_node_count), the children are the two
        // smallest-frequency slots in [0, k). Because we maintain
        // freq[0..k-1] in non-decreasing order, the two smallest are
        // simply at indices i and i+1; we then insertion-sort the new
        // combined slot into the partial-sorted region.
        std::size_t i = 0;
        for (std::size_t k = total_symbols; k < tree_node_count; ++k)
        {
          std::uint32_t f = static_cast<std::uint32_t>(freq[i])
                          + static_cast<std::uint32_t>(freq[i + 1]);
          // Find insertion position: largest L < k such that freq[L-1] <= f.
          std::size_t L = k;
          while (L > i + 2 && freq[L - 1] > f)
          {
            freq[L] = freq[L - 1];
            son[L] = son[L - 1];
            --L;
          }
          freq[L] = static_cast<std::uint16_t>(f);
          son[L] = static_cast<std::uint16_t>(i);
          i += 2;
        }

        // Re-derive parent pointers from son[].
        for (std::size_t kk = 0; kk < tree_node_count; ++kk)
        {
          if (son[kk] >= leaf_offset)
          {
            // Leaf: register reverse mapping.
            parent[son[kk]] = static_cast<std::uint16_t>(kk);
          }
          else
          {
            // Internal: both children point back to this slot.
            std::uint16_t left = son[kk];
            parent[left] = static_cast<std::uint16_t>(kk);
            parent[left + 1] = static_cast<std::uint16_t>(kk);
          }
        }
        freq[tree_node_count] = freq_sentinel;
        parent[root_slot] = 0;
      }

      // Update step run after every decoded symbol (spec section 4.3).
      void update(std::uint16_t symbol)
      {
        if (freq[root_slot] == freq_rebuild_threshold)
        {
          rebuild();
        }

        // Walk from the leaf for `symbol` upward to the root.
        std::uint16_t c = parent[leaf_offset + symbol];
        do
        {
          // Increment this slot's frequency.
          ++freq[c];
          std::uint16_t new_freq = freq[c];

          // If the new frequency now exceeds the slot immediately to
          // the right, locate the right-most slot whose old frequency
          // is still <= new_freq (scan forward while freq[l] is too
          // small). The sentinel at tree_node_count terminates the
          // scan harmlessly.
          std::uint16_t l = static_cast<std::uint16_t>(c + 1);
          if (freq[l] < new_freq)
          {
            while (freq[l + 1] < new_freq)
            {
              ++l;
            }

            // Swap subtrees at c and l: freq, son, and re-point the
            // parent pointers of the children of whichever slots have
            // moved (children of c becomes children of l and vice
            // versa).
            freq[c] = freq[l];
            freq[l] = new_freq;

            std::uint16_t son_c = son[c];
            std::uint16_t son_l = son[l];

            // Re-point children of (old c, new l) up to slot l.
            if (son_c >= leaf_offset)
            {
              parent[son_c] = l;
            }
            else
            {
              parent[son_c] = l;
              parent[son_c + 1] = l;
            }

            // Re-point children of (old l, new c) up to slot c.
            if (son_l >= leaf_offset)
            {
              parent[son_l] = c;
            }
            else
            {
              parent[son_l] = c;
              parent[son_l + 1] = c;
            }

            son[c] = son_l;
            son[l] = son_c;

            // Continue walking upward from the swap-target's parent.
            c = l;
          }

          c = parent[c];
        }
        while (c != 0);
      }

      // Decode a single Huffman symbol by walking the tree (spec 4.2).
      std::uint16_t decode_symbol(bit_reader& br) const
      {
        std::uint16_t c = son[root_slot];
        while (c < leaf_offset)
        {
          c = static_cast<std::uint16_t>(c + br.read_bits(1));
          c = son[c];
        }
        return static_cast<std::uint16_t>(c - leaf_offset);
      }
    };

    // Decode one 12-bit back-reference offset (spec section 5.2).
    // Steps strictly follow the spec's "operational" reading: read 8
    // bits as `i`, take top 6 = d_code[i], then read d_len[i] - 2 more
    // bits, shifting them into the low end of `i`; the resulting low 6
    // bits of `i` are the suffix.
    std::uint16_t decode_position(bit_reader& br)
    {
      std::uint16_t i = br.read_bits(8);
      std::uint16_t top6 = static_cast<std::uint16_t>(d_code[i] << 6);
      unsigned extra = static_cast<unsigned>(d_len[i]) - 2u;
      for (unsigned k = 0; k < extra; ++k)
      {
        std::uint16_t bit = br.read_bits(1);
        i = static_cast<std::uint16_t>(((i << 1) | bit) & 0xFFu);
      }
      return static_cast<std::uint16_t>(top6 | (i & 0x3Fu));
    }
  }

  std::vector<std::byte> lzh_decompress(
    std::istream& in,
    std::size_t expected_output_size)
  {
    std::vector<std::byte> output;
    output.reserve(expected_output_size);

    if (expected_output_size == 0)
    {
      return output;
    }

    // Initialise sliding window (spec section 6).
    std::array<std::uint8_t, window_size> window{};
    for (std::size_t i = 0; i < window_initial_cursor; ++i)
    {
      window[i] = window_fill_byte;
    }
    std::size_t write_cursor = window_initial_cursor;

    bit_reader br(in);
    huffman_tree tree;
    tree.init();

    while (output.size() < expected_output_size)
    {
      std::uint16_t s = tree.decode_symbol(br);
      if (s >= total_symbols)
      {
        throw std::runtime_error("lzh: Huffman symbol out of range");
      }
      tree.update(s);

      if (s < literal_alphabet)
      {
        // Literal byte.
        std::uint8_t b = static_cast<std::uint8_t>(s);
        output.push_back(static_cast<std::byte>(b));
        window[write_cursor] = b;
        write_cursor = (write_cursor + 1) & (window_size - 1);
      }
      else
      {
        // Back-reference: length = s - 253 (i.e. 3..60).
        std::size_t len = static_cast<std::size_t>(s)
                        - literal_alphabet
                        + copy_threshold;
        std::uint16_t off = decode_position(br);
        std::size_t src = (write_cursor + window_size - off - 1)
                          & (window_size - 1);
        for (std::size_t k = 0; k < len; ++k)
        {
          if (output.size() >= expected_output_size)
          {
            // Stop mid-back-reference; do not consume further input.
            return output;
          }
          std::uint8_t b = window[(src + k) & (window_size - 1)];
          output.push_back(static_cast<std::byte>(b));
          window[write_cursor] = b;
          write_cursor = (write_cursor + 1) & (window_size - 1);
        }
      }
    }

    return output;
  }

  std::vector<std::byte> lzh_decompress(
    const std::byte* data,
    std::size_t size,
    std::size_t expected_output_size)
  {
    // Stream-adapter over an in-memory buffer. We use a stringstream
    // built from a string view of the input to avoid a copy; the
    // payload is generally small enough that the convenience matters
    // more than the extra allocation.
    std::stringstream ss;
    ss.write(reinterpret_cast<const char*>(data),
             static_cast<std::streamsize>(size));
    return lzh_decompress(ss, expected_output_size);
  }
}
