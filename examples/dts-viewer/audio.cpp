// miniaudio is brought in as a single-header implementation here.  The
// header itself lives at third_party/miniaudio.h with the upstream
// public-domain / MIT-0 license footer preserved.  Defining
// MINIAUDIO_IMPLEMENTATION exactly once (this TU) pulls in the
// definitions.
#define MINIAUDIO_IMPLEMENTATION
// Trim out the parts we don't need to keep the TU compiling fast and
// limit the surface we're vendoring.  We need the core engine + device
// + decoder + audio_buffer for our use case.
#define MA_NO_GENERATION                 // no procedural waveform generators
#define MA_NO_NULL                       // no null backend
#include "third_party/miniaudio.h"

#include "audio.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace dts_viewer
{

namespace
{

// One playing voice.  Holds the converted-to-f32 PCM buffer (so we own
// the memory until the sound finishes), a miniaudio audio_buffer view
// over that PCM, and the ma_sound that pulls from the view.
struct Voice
{
    ma_sound            sound{};
    ma_audio_buffer     buffer{};      // owns its config; carries sample rate
    std::vector<float>  pcm_f32;       // backing store; buffer references it
    std::uint32_t       channels = 0;
    std::uint32_t       sample_rate = 0;
    bool                buffer_ok = false;
    bool                init_ok = false;
};

ma_engine                                              g_engine;
bool                                                   g_initialized = false;
std::atomic<SoundHandle>                               g_next_handle{1};
std::unordered_map<SoundHandle, std::unique_ptr<Voice>> g_voices;
std::mutex                                             g_voices_mutex;
glm::vec3 g_listener_pos { 0.0f };
glm::vec3 g_listener_fwd { 0.0f, 0.0f, 1.0f };
glm::vec3 g_listener_up  { 0.0f, 1.0f, 0.0f };

// Convert an interleaved PCM blob (8-bit unsigned or 16-bit signed
// little-endian) into normalised interleaved float32 frames.
std::vector<float> to_float32(const WavSample& s)
{
    const std::size_t bps = s.bits_per_sample;
    const std::size_t total_samples =
        (bps && s.channels)
            ? (s.pcm_data.size() / (bps / 8))
            : 0;
    std::vector<float> out(total_samples, 0.0f);
    if (bps == 8) {
        for (std::size_t i = 0; i < total_samples; ++i) {
            const std::uint8_t v = s.pcm_data[i];
            out[i] = (static_cast<float>(v) - 128.0f) / 128.0f;
        }
    } else if (bps == 16) {
        const std::uint8_t* p = s.pcm_data.data();
        for (std::size_t i = 0; i < total_samples; ++i) {
            const std::int16_t v = static_cast<std::int16_t>(
                p[i * 2] | (p[i * 2 + 1] << 8));
            out[i] = v / 32768.0f;
        }
    }
    return out;
}

// Garbage-collect voices whose sound has finished playing (one-shots).
// Called from each public entry-point so the voice map doesn't grow
// without bound across long missions.
void destroy_voice(Voice* v)
{
    if (v->init_ok)   ma_sound_uninit(&v->sound);
    if (v->buffer_ok) ma_audio_buffer_uninit(&v->buffer);
    v->init_ok   = false;
    v->buffer_ok = false;
}

void reap_finished_voices_locked()
{
    for (auto it = g_voices.begin(); it != g_voices.end(); ) {
        Voice* v = it->second.get();
        if (!v->init_ok) { ++it; continue; }
        if (!ma_sound_is_looping(&v->sound) &&
            ma_sound_at_end(&v->sound))
        {
            destroy_voice(v);
            it = g_voices.erase(it);
        } else {
            ++it;
        }
    }
}

// Build a Voice from a WavSample.  Returns a fully-initialised Voice,
// or one with buffer_ok=false when the sample is invalid.
std::unique_ptr<Voice> make_voice(const WavSample& sample)
{
    auto v = std::make_unique<Voice>();
    if (!sample.valid()) return v;
    v->pcm_f32     = to_float32(sample);
    v->channels    = sample.channels;
    v->sample_rate = sample.sample_rate;
    if (v->pcm_f32.empty()) return v;

    const ma_uint64 frames =
        v->pcm_f32.size() / std::max<ma_uint32>(1u, v->channels);
    ma_audio_buffer_config cfg = ma_audio_buffer_config_init(
        ma_format_f32, v->channels, frames,
        v->pcm_f32.data(), nullptr);
    cfg.sampleRate = v->sample_rate;
    if (ma_audio_buffer_init(&cfg, &v->buffer) != MA_SUCCESS) return v;
    v->buffer_ok = true;
    return v;
}

ma_result init_sound_from_voice(Voice* v, bool looping, bool positional)
{
    if (!v->buffer_ok) return MA_INVALID_OPERATION;
    const ma_uint32 flags =
        positional ? 0u : MA_SOUND_FLAG_NO_SPATIALIZATION;
    ma_result r = ma_sound_init_from_data_source(
        &g_engine, &v->buffer, flags, nullptr, &v->sound);
    if (r != MA_SUCCESS) return r;
    ma_sound_set_looping(&v->sound, looping ? MA_TRUE : MA_FALSE);
    v->init_ok = true;
    return MA_SUCCESS;
}

} // anonymous namespace

bool audio_init()
{
    if (g_initialized) return true;
    ma_engine_config cfg = ma_engine_config_init();
    cfg.sampleRate = 44100;
    ma_result r = ma_engine_init(&cfg, &g_engine);
    if (r != MA_SUCCESS) {
        std::fprintf(stderr,
            "audio: ma_engine_init failed (%d) — falling back to silent stub\n",
            static_cast<int>(r));
        g_initialized = false;
        return false;
    }
    g_initialized = true;
    std::fprintf(stderr,
        "audio: miniaudio backend up — sample_rate=%u channels=%u\n",
        ma_engine_get_sample_rate(&g_engine),
        ma_engine_get_channels(&g_engine));
    return true;
}

void audio_shutdown()
{
    if (!g_initialized) return;
    {
        std::lock_guard<std::mutex> lock(g_voices_mutex);
        for (auto& kv : g_voices) destroy_voice(kv.second.get());
        g_voices.clear();
    }
    ma_engine_uninit(&g_engine);
    g_initialized = false;
}

SoundHandle audio_play_oneshot(const WavSample& sample, float gain)
{
    if (!g_initialized) return kInvalidSound;
    auto v = make_voice(sample);
    if (v->pcm_f32.empty()) return kInvalidSound;
    if (init_sound_from_voice(v.get(), /*looping*/false, /*positional*/false) != MA_SUCCESS)
        return kInvalidSound;
    ma_sound_set_volume(&v->sound, gain);
    ma_sound_start(&v->sound);
    SoundHandle h = g_next_handle.fetch_add(1);
    std::lock_guard<std::mutex> lock(g_voices_mutex);
    reap_finished_voices_locked();
    g_voices.emplace(h, std::move(v));
    return h;
}

SoundHandle audio_play_looping(const WavSample& sample, float gain)
{
    if (!g_initialized) return kInvalidSound;
    auto v = make_voice(sample);
    if (v->pcm_f32.empty()) return kInvalidSound;
    if (init_sound_from_voice(v.get(), /*looping*/true, /*positional*/false) != MA_SUCCESS)
        return kInvalidSound;
    ma_sound_set_volume(&v->sound, gain);
    ma_sound_start(&v->sound);
    SoundHandle h = g_next_handle.fetch_add(1);
    std::lock_guard<std::mutex> lock(g_voices_mutex);
    g_voices.emplace(h, std::move(v));
    return h;
}

void audio_stop(SoundHandle h)
{
    if (!g_initialized || h == kInvalidSound) return;
    std::lock_guard<std::mutex> lock(g_voices_mutex);
    auto it = g_voices.find(h);
    if (it == g_voices.end()) return;
    destroy_voice(it->second.get());
    g_voices.erase(it);
}

void audio_set_listener(const glm::vec3& pos,
                        const glm::vec3& forward,
                        const glm::vec3& up)
{
    g_listener_pos = pos;
    g_listener_fwd = forward;
    g_listener_up  = up;
    if (!g_initialized) return;
    ma_engine_listener_set_position(&g_engine, 0, pos.x, pos.y, pos.z);
    ma_engine_listener_set_direction(&g_engine, 0,
        forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&g_engine, 0, up.x, up.y, up.z);
}

SoundHandle audio_play_at(const WavSample& sample,
                          const glm::vec3& world_pos,
                          float ref_distance,
                          float max_distance,
                          float gain,
                          bool  looping)
{
    if (!g_initialized) return kInvalidSound;
    auto v = make_voice(sample);
    if (v->pcm_f32.empty()) return kInvalidSound;
    if (init_sound_from_voice(v.get(), looping, /*positional*/true) != MA_SUCCESS)
        return kInvalidSound;
    ma_sound_set_volume(&v->sound, gain);
    ma_sound_set_position(&v->sound, world_pos.x, world_pos.y, world_pos.z);
    ma_sound_set_attenuation_model(&v->sound, ma_attenuation_model_inverse);
    ma_sound_set_min_distance(&v->sound, ref_distance);
    ma_sound_set_max_distance(&v->sound, max_distance);
    ma_sound_start(&v->sound);
    SoundHandle h = g_next_handle.fetch_add(1);
    std::lock_guard<std::mutex> lock(g_voices_mutex);
    reap_finished_voices_locked();
    g_voices.emplace(h, std::move(v));
    return h;
}

void audio_set_source_position(SoundHandle h, const glm::vec3& world_pos)
{
    if (!g_initialized || h == kInvalidSound) return;
    std::lock_guard<std::mutex> lock(g_voices_mutex);
    auto it = g_voices.find(h);
    if (it == g_voices.end() || !it->second->init_ok) return;
    ma_sound_set_position(&it->second->sound, world_pos.x, world_pos.y, world_pos.z);
}

bool audio_backend_active() { return g_initialized; }

namespace
{

struct MiniAudioSink final : IAudioSink
{
    SoundHandle play_oneshot(const WavSample& s, float g) override
    { return audio_play_oneshot(s, g); }
    SoundHandle play_looping(const WavSample& s, float g) override
    { return audio_play_looping(s, g); }
    SoundHandle play_at(const WavSample& s, const glm::vec3& p,
                        float rd, float md, float g, bool l) override
    { return audio_play_at(s, p, rd, md, g, l); }
    void stop(SoundHandle h) override { audio_stop(h); }
    void set_source_position(SoundHandle h, const glm::vec3& p) override
    { audio_set_source_position(h, p); }
    void set_listener(const glm::vec3& p, const glm::vec3& f, const glm::vec3& u) override
    { audio_set_listener(p, f, u); }
    bool active() const override { return audio_backend_active(); }
};

} // anonymous namespace

IAudioSink& audio_default_sink()
{
    static MiniAudioSink s;
    return s;
}

} // namespace dts_viewer
