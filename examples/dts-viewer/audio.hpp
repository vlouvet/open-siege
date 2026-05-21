#ifndef DTS_VIEWER_AUDIO_HPP
#define DTS_VIEWER_AUDIO_HPP

// Audio backend API — Track 11 specs 02–06.
//
// v1 ships the API surface only; the actual mixing/output backend (miniaudio
// or OpenAL Soft) requires vendoring a third-party header which is gated on
// user authorisation.  Until that happens, audio_init() returns true and
// every play_* call returns a non-zero handle but no audio is emitted.  All
// callers continue to function (turret loops, BGM, footsteps), they're just
// silent.
//
// When the backend is wired (a future spec or interactive vendor step), only
// audio.cpp changes — callers depend on this header alone.

#include <cstdint>
#include <glm/glm.hpp>

#include "wav_loader.hpp"

namespace dts_viewer
{

using SoundHandle = std::uint32_t;
constexpr SoundHandle kInvalidSound = 0;

// One-shot lifecycle.
bool          audio_init();
void          audio_shutdown();

// 2D playback (no positional attenuation).
SoundHandle   audio_play_oneshot(const WavSample& sample, float gain = 1.0f);
SoundHandle   audio_play_looping(const WavSample& sample, float gain = 1.0f);
void          audio_stop(SoundHandle h);

// 3D positional (spec 11/03).
void          audio_set_listener(const glm::vec3& pos,
                                 const glm::vec3& forward,
                                 const glm::vec3& up);
SoundHandle   audio_play_at(const WavSample& sample,
                            const glm::vec3& world_pos,
                            float ref_distance = 8.0f,
                            float max_distance = 100.0f,
                            float gain = 1.0f,
                            bool  looping = false);
void          audio_set_source_position(SoundHandle h, const glm::vec3& world_pos);

// Backend health (for stderr logging).  Returns false when the stub backend
// is active; true when a real backend has been wired.
bool          audio_backend_active();

} // namespace dts_viewer

#endif // DTS_VIEWER_AUDIO_HPP
