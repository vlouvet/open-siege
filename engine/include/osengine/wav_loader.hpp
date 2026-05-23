#ifndef DTS_VIEWER_WAV_LOADER_HPP
#define DTS_VIEWER_WAV_LOADER_HPP

// RIFF WAV loader — Track 11 spec 01.
//
// Parses standard PCM WAV files (RIFF / WAVE / fmt / data chunks).  No
// dependency on libsndfile or similar — RIFF is one of the simplest
// formats; ~80 lines of chunk-walking suffice for every shipping Tribes
// audio asset (verified PCM 16-bit mono, mostly 22050 Hz).

#include <cstdint>
#include <istream>
#include <optional>
#include <span>
#include <vector>

namespace dts_viewer
{

struct WavSample
{
    std::uint32_t sample_rate    = 0;
    std::uint16_t channels       = 0;
    std::uint16_t bits_per_sample = 0;
    std::vector<std::uint8_t> pcm_data;   // interleaved PCM, little-endian

    bool valid() const
    {
        return sample_rate > 0 && channels > 0 &&
               (bits_per_sample == 8  || bits_per_sample == 16) &&
               !pcm_data.empty();
    }

    std::size_t frame_count() const
    {
        const std::size_t frame_bytes =
            static_cast<std::size_t>(channels) * (bits_per_sample / 8);
        return frame_bytes ? (pcm_data.size() / frame_bytes) : 0;
    }
};

std::optional<WavSample> load_wav(std::istream& in);

// Span overload for in-memory buffers (e.g. VOL extraction).
std::optional<WavSample> load_wav(const std::uint8_t* data, std::size_t size);

} // namespace dts_viewer

#endif // DTS_VIEWER_WAV_LOADER_HPP
