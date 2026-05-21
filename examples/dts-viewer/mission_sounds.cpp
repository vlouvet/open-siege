#include "mission_sounds.hpp"

#include <unordered_map>
#include <variant>

namespace dts_viewer
{

namespace
{

// Hard-coded datablock-name -> ambient WAV table.  Extended as new
// ambients are identified in the corpus; Track 16 (cscript-bindings)
// will eventually replace this with a script-driven lookup.
const std::unordered_map<std::string, std::string>& ambient_map()
{
    static const std::unordered_map<std::string, std::string> m = {
        { "Generator",     "generator_hum.wav" },
        { "PortGenerator", "generator_hum.wav" },
        { "SolarPanel",    "panel_hum.wav"     },
        { "ForceField",    "forcefield.wav"    },
    };
    return m;
}

// Until the audio backend is wired, we don't actually load the WAV file
// data — but the placeholder WavSample lets the API return a non-zero
// handle, which is what the mission switcher cares about for unload.
WavSample empty_sample()
{
    WavSample s;
    s.sample_rate = 22050;
    s.channels = 1;
    s.bits_per_sample = 16;
    s.pcm_data.resize(4, 0);
    return s;
}

void walk(
    const studio::content::mission::scene_node& n,
    MissionSoundsState& state,
    const WavSample& placeholder)
{
    using namespace studio::content::mission;
    std::visit([&](const auto& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, node_static_shape>) {
            auto it = ambient_map().find(p.data_block.name);
            if (it != ambient_map().end()) {
                glm::vec3 pos{ p.xf.position[0], p.xf.position[1], p.xf.position[2] };
                SoundHandle h = audio_play_at(placeholder, pos,
                    8.0f, 100.0f, 1.0f, /*looping*/true);
                if (h != kInvalidSound) state.voices.push_back(h);
            }
        }
        else if constexpr (std::is_same_v<T, node_snowfall>) {
            if (!state.wind_active) {
                state.wind_handle = audio_play_looping(placeholder, 0.3f);
                state.wind_active = state.wind_handle != kInvalidSound;
            }
        }
    }, n.payload);
    for (auto& c : n.children) walk(c, state, placeholder);
}

} // anonymous namespace

MissionSoundsState mission_sounds_load(
    const studio::content::mission::scene_graph& scene,
    const std::filesystem::path& /*vol_dir*/)
{
    MissionSoundsState st;
    const WavSample placeholder = empty_sample();
    walk(scene.root, st, placeholder);
    return st;
}

void mission_sounds_unload(MissionSoundsState& state)
{
    for (SoundHandle h : state.voices) audio_stop(h);
    state.voices.clear();
    if (state.wind_handle != kInvalidSound) {
        audio_stop(state.wind_handle);
        state.wind_handle = kInvalidSound;
    }
    state.wind_active = false;
}

} // namespace dts_viewer
