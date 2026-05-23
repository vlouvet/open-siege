#include <osengine/audio_sink.hpp>

namespace dts_viewer
{

namespace
{

struct NullAudioSink final : IAudioSink
{
    SoundHandle play_oneshot(const WavSample&, float) override { return kInvalidSound; }
    SoundHandle play_looping(const WavSample&, float) override { return kInvalidSound; }
    SoundHandle play_at(const WavSample&, const glm::vec3&,
                        float, float, float, bool) override { return kInvalidSound; }
    void stop(SoundHandle) override {}
    void set_source_position(SoundHandle, const glm::vec3&) override {}
    void set_listener(const glm::vec3&, const glm::vec3&, const glm::vec3&) override {}
    bool active() const override { return false; }
};

} // anonymous namespace

IAudioSink& null_audio_sink()
{
    static NullAudioSink s;
    return s;
}

} // namespace dts_viewer
