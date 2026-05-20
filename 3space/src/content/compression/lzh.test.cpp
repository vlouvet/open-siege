// Unit tests for the clean-room LZH decoder. The hard-coded test
// vectors come from docs/clean-room-specs/LZH-CODEC.md section 7 and
// are derived from real shipping Tribes 1.41 freeware mission files.
// SHA-256 digests are pinned so any regression in the decoder will
// trip the byte-exact check.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch.hpp>

#include "content/compression/lzh.hpp"

namespace
{
  // Minimal SHA-256 implementation so the test does not pull in a new
  // dependency just to verify the spec's hash-pinned vectors.
  class sha256
  {
  public:
    sha256() { reset(); }

    void update(const std::byte* data, std::size_t len)
    {
      const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(data);
      while (len > 0)
      {
        std::size_t take = std::min<std::size_t>(len, 64 - buf_len_);
        std::memcpy(buf_ + buf_len_, p, take);
        buf_len_ += take;
        p += take;
        len -= take;
        total_ += take;
        if (buf_len_ == 64)
        {
          transform();
          buf_len_ = 0;
        }
      }
    }

    std::array<std::uint8_t, 32> finalize()
    {
      std::uint64_t bit_len = total_ * 8;
      buf_[buf_len_++] = 0x80;
      if (buf_len_ > 56)
      {
        while (buf_len_ < 64) buf_[buf_len_++] = 0;
        transform();
        buf_len_ = 0;
      }
      while (buf_len_ < 56) buf_[buf_len_++] = 0;
      for (int i = 7; i >= 0; --i)
      {
        buf_[buf_len_++] = static_cast<std::uint8_t>((bit_len >> (i * 8)) & 0xFF);
      }
      transform();
      std::array<std::uint8_t, 32> out{};
      for (int i = 0; i < 8; ++i)
      {
        out[i * 4 + 0] = static_cast<std::uint8_t>((h_[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<std::uint8_t>((h_[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<std::uint8_t>((h_[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<std::uint8_t>(h_[i] & 0xFF);
      }
      return out;
    }

  private:
    void reset()
    {
      h_[0] = 0x6a09e667; h_[1] = 0xbb67ae85; h_[2] = 0x3c6ef372; h_[3] = 0xa54ff53a;
      h_[4] = 0x510e527f; h_[5] = 0x9b05688c; h_[6] = 0x1f83d9ab; h_[7] = 0x5be0cd19;
      buf_len_ = 0;
      total_ = 0;
    }

    static std::uint32_t rotr(std::uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

    void transform()
    {
      static const std::uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
      };
      std::uint32_t w[64];
      for (int i = 0; i < 16; ++i)
      {
        w[i] = (std::uint32_t(buf_[i * 4]) << 24)
             | (std::uint32_t(buf_[i * 4 + 1]) << 16)
             | (std::uint32_t(buf_[i * 4 + 2]) << 8)
             | (std::uint32_t(buf_[i * 4 + 3]));
      }
      for (int i = 16; i < 64; ++i)
      {
        std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
      }
      std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
      std::uint32_t e = h_[4], f = h_[5], g = h_[6], hh = h_[7];
      for (int i = 0; i < 64; ++i)
      {
        std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        std::uint32_t ch = (e & f) ^ (~e & g);
        std::uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        std::uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
      }
      h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
      h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += hh;
    }

    std::uint32_t h_[8];
    std::uint8_t buf_[64];
    std::size_t buf_len_;
    std::uint64_t total_;
  };

  std::string hex_of(const std::byte* data, std::size_t len)
  {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i)
    {
      auto b = static_cast<std::uint8_t>(data[i]);
      s.push_back(digits[b >> 4]);
      s.push_back(digits[b & 0xF]);
    }
    return s;
  }

  std::string sha256_hex(const std::vector<std::byte>& data)
  {
    sha256 h;
    h.update(data.data(), data.size());
    auto digest = h.finalize();
    std::string s;
    static const char* digits = "0123456789abcdef";
    for (auto b : digest)
    {
      s.push_back(digits[b >> 4]);
      s.push_back(digits[b & 0xF]);
    }
    return s;
  }

  // Load the LZH-compressed heightmap region of one of the test
  // mission DTBs into a byte buffer. Returns an empty vector (and the
  // caller skips the test) if the sample isn't present on disk —
  // tests should still build and pass on contributor machines that
  // don't have the Tribes asset corpus.
  std::vector<std::byte> load_dtb_compressed(const char* sample_path,
                                              std::size_t& declared_uncompressed)
  {
    std::ifstream f(sample_path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto size = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    std::vector<std::byte> buf(size);
    f.read(reinterpret_cast<char*>(buf.data()),
           static_cast<std::streamsize>(size));
    if (buf.size() < 56) return {};
    // u32 LE at offset 52 == declared uncompressed size.
    std::uint32_t sz = 0;
    for (int i = 0; i < 4; ++i)
    {
      sz |= std::uint32_t(static_cast<std::uint8_t>(buf[52 + i])) << (i * 8);
    }
    declared_uncompressed = sz;
    return std::vector<std::byte>(buf.begin() + 56, buf.end());
  }
}

using studio::content::compression::lzh_decompress;

TEST_CASE("LZH decoder produces 0x7F as the first decoded byte (Vector 5)",
          "[compression.lzh]")
{
  // Smoke-test from spec section 7 Vector 5. If the decoder
  // mistakenly pulls a back-reference into the initial-space-fill
  // window, the first byte will be 0x20 instead.
  std::size_t declared = 0;
  auto compressed = load_dtb_compressed(
    "/tmp/lzh-samples/welcome/1_Welcome#0.dtb", declared);
  if (compressed.empty())
  {
    WARN("sample 1_Welcome#0.dtb not present at /tmp/lzh-samples/welcome/ — "
         "extract first via vol-list");
    return;
  }
  auto out = lzh_decompress(compressed.data(), compressed.size(), 1);
  REQUIRE(out.size() == 1);
  REQUIRE(static_cast<std::uint8_t>(out[0]) == 0x7F);
}

TEST_CASE("LZH decoder honours expected_size as the sole stop condition "
          "(Vector 4)",
          "[compression.lzh]")
{
  std::size_t declared = 0;
  auto compressed = load_dtb_compressed(
    "/tmp/lzh-samples/welcome/1_Welcome#0.dtb", declared);
  if (compressed.empty())
  {
    WARN("sample missing; see above");
    return;
  }

  struct prefix_case
  {
    std::size_t n;
    const char* expected_hex;
  };

  const prefix_case cases[] = {
    {1, "7f"},
    {4, "7f71ce42"},
    {16, "7f71ce4274e1d04203ebd2424fe7d842"},
    {64, "7f71ce4274e1d04203ebd2424fe7d842"
         "004adf42a2a7e342de9ee74264a2ec42"
         "500cf2420c79f94217a60043fa440443"
         "aab0074346430a43afa20c4354650d43"},
  };
  for (auto const& tc : cases)
  {
    auto out = lzh_decompress(compressed.data(), compressed.size(), tc.n);
    REQUIRE(out.size() == tc.n);
    REQUIRE(hex_of(out.data(), out.size()) == std::string(tc.expected_hex));
  }
}

TEST_CASE("LZH decoder reproduces Vector 1 (1_Welcome heightmap)",
          "[compression.lzh][vector]")
{
  std::size_t declared = 0;
  auto compressed = load_dtb_compressed(
    "/tmp/lzh-samples/welcome/1_Welcome#0.dtb", declared);
  if (compressed.empty()) { WARN("sample missing"); return; }
  REQUIRE(declared == 264196);

  auto out = lzh_decompress(compressed.data(), compressed.size(), declared);
  REQUIRE(out.size() == declared);

  // Sanity: the float range encoded in the heightmap must lie within
  // the [fMin, fMax] header field. Stronger than that — for this
  // sample the min/max read as 52.358238 and 193.659775 per the spec.
  // We verify only the SHA-256 here; the float-range invariant is
  // implicit in the SHA match.
  REQUIRE(sha256_hex(out)
          == "224de8506691645686ef409b440694766bb60c8aef7b666d75f6279c02a173e3");

  REQUIRE(hex_of(out.data(), 32)
          == "7f71ce4274e1d04203ebd2424fe7d842"
             "004adf42a2a7e342de9ee74264a2ec42");
  REQUIRE(hex_of(out.data() + out.size() - 16, 16)
          == "a5bbc64289d5c642b7d6ca427f71ce42");
}

TEST_CASE("LZH decoder reproduces Vector 2 (3_Vehicle heightmap)",
          "[compression.lzh][vector]")
{
  std::size_t declared = 0;
  auto compressed = load_dtb_compressed(
    "/tmp/lzh-samples/vehicle/3_Vehicle#0.dtb", declared);
  if (compressed.empty()) { WARN("sample missing"); return; }
  REQUIRE(declared == 264196);

  auto out = lzh_decompress(compressed.data(), compressed.size(), declared);
  REQUIRE(out.size() == declared);
  REQUIRE(sha256_hex(out)
          == "ab725f9c53877be99bf42da66116f031c8cee49e30cd33b70b4b9f8c665e055e");
  REQUIRE(hex_of(out.data(), 32)
          == "3feb4b435ad84a43b78c49436a0e48430c804643cb0545430cae434398614243");
  REQUIRE(hex_of(out.data() + out.size() - 16, 16)
          == "03f24e43dae54d43b5da4c433feb4b43");
}

TEST_CASE("LZH decoder reproduces Vector 3 (5_CTF heightmap)",
          "[compression.lzh][vector]")
{
  std::size_t declared = 0;
  auto compressed = load_dtb_compressed(
    "/tmp/lzh-samples/ctf/5_CTF#0.dtb", declared);
  if (compressed.empty()) { WARN("sample missing"); return; }
  REQUIRE(declared == 264196);

  auto out = lzh_decompress(compressed.data(), compressed.size(), declared);
  REQUIRE(out.size() == declared);
  REQUIRE(sha256_hex(out)
          == "b4e5213338c9efa531047ffe62de4c6f046eaff27b1f572414f1bd1c90501ae7");
  REQUIRE(hex_of(out.data(), 32)
          == "f1ef48439a984a43ddda4c4353504e43c9c54e43a6a14f43827d4f435f594f43");
  REQUIRE(hex_of(out.data() + out.size() - 16, 16)
          == "da69b9428e9bb74242cdb542f6feb142");
}

TEST_CASE("LZH decoder accepts an std::istream input directly",
          "[compression.lzh]")
{
  std::size_t declared = 0;
  auto compressed = load_dtb_compressed(
    "/tmp/lzh-samples/welcome/1_Welcome#0.dtb", declared);
  if (compressed.empty()) { WARN("sample missing"); return; }

  std::stringstream ss;
  ss.write(reinterpret_cast<const char*>(compressed.data()),
           static_cast<std::streamsize>(compressed.size()));
  auto out = lzh_decompress(ss, declared);
  REQUIRE(out.size() == declared);
  REQUIRE(static_cast<std::uint8_t>(out[0]) == 0x7F);
}

TEST_CASE("LZH decoder decodes broader DTB corpus to plausible heightmap data",
          "[compression.lzh][corpus]")
{
  // Beyond the three hash-pinned vectors, exercise additional real
  // .dtb files. The plausibility check is: 257 * 257 = 66 049 little-
  // endian f32 height samples must each fall within the [fMin, fMax]
  // range stored in the DTB's GBLK header at file offset 36 (two
  // floats). This catches subtle decoder bugs (e.g. tree-state drift
  // that produces structurally valid but wrong byte values).

  struct corpus_entry
  {
    const char* path;
    float fmin;
    float fmax;
  };

  const corpus_entry corpus[] = {
    {"/tmp/lzh-samples/2_weapons/2_Weapons#0.dtb",
     152.626404f, 199.980392f},
    {"/tmp/lzh-samples/4_commander_targetlaser/"
     "4_Commander_TargetLaser#0.dtb",
     4.142991f, 106.381454f},
    {"/tmp/lzh-samples/6_towers/6_Towers#0.dtb",
     6.804935f, 177.187897f},
    {"/tmp/lzh-samples/7_retrieval/7_Retrieval#0.dtb",
     152.626328f, 199.980377f},
    {"/tmp/lzh-samples/8_destroy/8_Destroy#0.dtb",
     100.0f, 289.139587f},
  };

  std::size_t decoded_count = 0;
  for (auto const& e : corpus)
  {
    std::size_t declared = 0;
    auto compressed = load_dtb_compressed(e.path, declared);
    if (compressed.empty())
    {
      WARN(std::string("sample missing: ") + e.path);
      continue;
    }
    REQUIRE(declared == 264196u);   // 257 * 257 * 4

    auto out = lzh_decompress(compressed.data(), compressed.size(), declared);
    REQUIRE(out.size() == declared);

    // Reinterpret as 66 049 f32s. Confirm in-range.
    std::size_t const n = declared / 4;
    float local_min = std::numeric_limits<float>::infinity();
    float local_max = -std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < n; ++i)
    {
      float v;
      std::memcpy(&v, out.data() + i * 4, 4);
      if (v < local_min) local_min = v;
      if (v > local_max) local_max = v;
    }
    // Allow a small slack: the GBLK header records the actual min/max
    // of the decoded heights, but use an epsilon relative to the
    // range to absorb f32 rounding noise.
    float const range = e.fmax - e.fmin;
    float const eps = std::max(range * 1e-4f, 1e-4f);
    REQUIRE(local_min >= e.fmin - eps);
    REQUIRE(local_max <= e.fmax + eps);
    // And the decoded values must actually use the range — degenerate
    // output (e.g. all zeros) would fail this.
    REQUIRE(local_max - local_min > range * 0.5f);
    ++decoded_count;
  }

  if (decoded_count == 0)
  {
    WARN("no DTB samples present; corpus test effectively skipped");
  }
}
