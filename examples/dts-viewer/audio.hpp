#ifndef DTS_VIEWER_AUDIO_HPP
#define DTS_VIEWER_AUDIO_HPP

// dts-viewer's miniaudio backend.
//
// The pure-virtual IAudioSink lives in <osengine/audio_sink.hpp> so engine
// code (mission_sounds, weapons, ...) can call it without dragging the
// miniaudio header in. main.cpp still uses the free-function init/shutdown
// + set_listener for the top-level lifecycle; the sink wraps everything.

#include <osengine/audio_sink.hpp>
#include <osengine/wav_loader.hpp>

#include <glm/glm.hpp>

namespace dts_viewer
{

// One-shot lifecycle.
bool          audio_init();
void          audio_shutdown();

// The MiniAudioSink for the running backend. mission_sounds_load receives
// this; pass null_audio_sink() instead when audio_init failed or you want
// no audio.
IAudioSink&   audio_default_sink();

// 2D playback (no positional attenuation).
SoundHandle   audio_play_oneshot(const WavSample& sample, float gain = 1.0f);
SoundHandle   audio_play_looping(const WavSample& sample, float gain = 1.0f);
void          audio_stop(SoundHandle h);

// 3D positional.
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

// Backend health (for stderr logging).
bool          audio_backend_active();

} // namespace dts_viewer

#endif // DTS_VIEWER_AUDIO_HPP
