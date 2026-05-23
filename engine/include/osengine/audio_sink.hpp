#ifndef OSENGINE_AUDIO_SINK_HPP
#define OSENGINE_AUDIO_SINK_HPP

// Pluggable audio backend — spec 26/04.
//
// Engine code (mission_sounds.cpp, weapons.cpp, ...) decides WHEN sounds
// happen; the actual playback (miniaudio device, mixer, 3D fold-down) is
// the consumer's job. dts-viewer + open-siege-t1-client use the
// MiniAudioSink in apps/dts-viewer; open-siege-t1-server uses
// NullAudioSink (server still decides events for replication, just drops
// the play call).

#include <cstdint>
#include <glm/glm.hpp>

namespace dts_viewer { struct WavSample; }

namespace dts_viewer
{

using SoundHandle = std::uint32_t;
constexpr SoundHandle kInvalidSound = 0;

struct IAudioSink
{
    virtual ~IAudioSink() = default;

    virtual SoundHandle play_oneshot(const WavSample& sample, float gain) = 0;
    virtual SoundHandle play_looping(const WavSample& sample, float gain) = 0;
    virtual SoundHandle play_at(const WavSample& sample,
                                const glm::vec3& world_pos,
                                float ref_distance,
                                float max_distance,
                                float gain,
                                bool  looping) = 0;
    virtual void stop(SoundHandle h) = 0;
    virtual void set_source_position(SoundHandle h, const glm::vec3& world_pos) = 0;
    virtual void set_listener(const glm::vec3& pos,
                              const glm::vec3& forward,
                              const glm::vec3& up) = 0;
    virtual bool active() const = 0;
};

// Singleton no-op sink — for the server binary, for unit tests, for any
// caller that has no audio output. All play_* calls return kInvalidSound.
IAudioSink& null_audio_sink();

} // namespace dts_viewer

#endif // OSENGINE_AUDIO_SINK_HPP
