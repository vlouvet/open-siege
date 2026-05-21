#include "wav_loader.hpp"

#include <cstdio>
#include <cstring>
#include <sstream>

namespace dts_viewer
{

namespace
{

std::uint32_t read_u32_le(std::istream& in)
{
    std::uint8_t b[4]{};
    in.read(reinterpret_cast<char*>(b), 4);
    return  static_cast<std::uint32_t>(b[0])        |
           (static_cast<std::uint32_t>(b[1]) << 8 ) |
           (static_cast<std::uint32_t>(b[2]) << 16) |
           (static_cast<std::uint32_t>(b[3]) << 24);
}

std::uint16_t read_u16_le(std::istream& in)
{
    std::uint8_t b[2]{};
    in.read(reinterpret_cast<char*>(b), 2);
    return static_cast<std::uint16_t>(b[0] | (b[1] << 8));
}

bool match_tag(std::istream& in, const char tag[4])
{
    char buf[4]{};
    in.read(buf, 4);
    return std::memcmp(buf, tag, 4) == 0;
}

} // anonymous namespace

std::optional<WavSample> load_wav(std::istream& in)
{
    if (!match_tag(in, "RIFF")) return std::nullopt;
    const std::uint32_t riff_size = read_u32_le(in);
    (void)riff_size;
    if (!match_tag(in, "WAVE")) return std::nullopt;

    WavSample out;
    bool have_fmt  = false;
    bool have_data = false;

    while (in && !have_data) {
        char tag[4]{};
        in.read(tag, 4);
        if (in.gcount() != 4) break;
        std::uint32_t chunk_size = read_u32_le(in);
        if (chunk_size == 0xFFFFFFFFu) break;

        if (std::memcmp(tag, "fmt ", 4) == 0) {
            const std::streampos chunk_start = in.tellg();
            const std::uint16_t fmt_code = read_u16_le(in);    // 1 = PCM
            out.channels        = read_u16_le(in);
            out.sample_rate     = read_u32_le(in);
            (void)read_u32_le(in);                              // byte_rate
            (void)read_u16_le(in);                              // block_align
            out.bits_per_sample = read_u16_le(in);
            if (fmt_code != 1) {
                std::fprintf(stderr,
                    "wav: non-PCM format code %u — unsupported\n", fmt_code);
                return std::nullopt;
            }
            // Skip any trailing fmt chunk bytes (extensible WAVs have more).
            const std::uint32_t consumed = static_cast<std::uint32_t>(
                in.tellg() - chunk_start);
            if (consumed < chunk_size) {
                in.seekg(chunk_size - consumed, std::ios::cur);
            }
            // Pad to word alignment.
            if (chunk_size & 1u) in.seekg(1, std::ios::cur);
            have_fmt = true;
        }
        else if (std::memcmp(tag, "data", 4) == 0) {
            out.pcm_data.resize(chunk_size);
            in.read(reinterpret_cast<char*>(out.pcm_data.data()), chunk_size);
            if (chunk_size & 1u) in.seekg(1, std::ios::cur);
            have_data = true;
        }
        else {
            // Skip unknown chunk.
            in.seekg(chunk_size + (chunk_size & 1u), std::ios::cur);
        }
    }

    if (!have_fmt || !have_data) return std::nullopt;
    if (!out.valid())            return std::nullopt;
    return out;
}

std::optional<WavSample> load_wav(const std::uint8_t* data, std::size_t size)
{
    std::stringstream ss(std::string(
        reinterpret_cast<const char*>(data), size));
    return load_wav(ss);
}

} // namespace dts_viewer
