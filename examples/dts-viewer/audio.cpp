#include "audio.hpp"

#include <atomic>
#include <cstdio>

namespace dts_viewer
{

// Stub audio backend.  See audio.hpp for the deferral rationale.  All
// play_* calls return a fresh handle; nothing is actually mixed or
// emitted.  When a real backend (miniaudio / OpenAL Soft) is vendored,
// this file's body is the only thing that needs to change.

namespace
{
std::atomic<SoundHandle> g_next_handle { 1 };
bool g_initialized = false;
glm::vec3 g_listener_pos { 0.0f };
glm::vec3 g_listener_fwd { 0.0f, 0.0f, 1.0f };
glm::vec3 g_listener_up  { 0.0f, 1.0f, 0.0f };
} // anonymous namespace

bool audio_init()
{
    g_initialized = true;
    std::fprintf(stderr,
        "audio: stub backend active (vendored mixer deferred; "
        "see Track 11 spec 02 notes)\n");
    return true;
}

void audio_shutdown()
{
    g_initialized = false;
}

SoundHandle audio_play_oneshot(const WavSample& /*sample*/, float /*gain*/)
{
    if (!g_initialized) return kInvalidSound;
    return g_next_handle.fetch_add(1);
}

SoundHandle audio_play_looping(const WavSample& /*sample*/, float /*gain*/)
{
    if (!g_initialized) return kInvalidSound;
    return g_next_handle.fetch_add(1);
}

void audio_stop(SoundHandle /*h*/)
{
    // no-op
}

void audio_set_listener(const glm::vec3& pos,
                        const glm::vec3& forward,
                        const glm::vec3& up)
{
    g_listener_pos = pos;
    g_listener_fwd = forward;
    g_listener_up  = up;
}

SoundHandle audio_play_at(const WavSample& /*sample*/,
                          const glm::vec3& /*world_pos*/,
                          float /*ref_distance*/,
                          float /*max_distance*/,
                          float /*gain*/,
                          bool  /*looping*/)
{
    if (!g_initialized) return kInvalidSound;
    return g_next_handle.fetch_add(1);
}

void audio_set_source_position(SoundHandle /*h*/, const glm::vec3& /*world_pos*/)
{
    // no-op (stub)
}

bool audio_backend_active() { return false; }

} // namespace dts_viewer
