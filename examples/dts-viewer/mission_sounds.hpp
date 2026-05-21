#ifndef DTS_VIEWER_MISSION_SOUNDS_HPP
#define DTS_VIEWER_MISSION_SOUNDS_HPP

// Mission-defined ambient sounds — Track 11 spec 05.
//
// Walks a loaded mission's scene graph for `node_static_shape` instances
// whose dataBlock name maps to an ambient WAV (generator hum, fan loop,
// etc.) and for `node_snowfall` entries (drives a global wind ambient).
// Audio is routed through `audio.hpp`; while the stub backend is active
// the calls are silent but the bookkeeping still runs so the mission
// switcher can stop/restart cleanly.

#include "audio.hpp"
#include "content/mission/scene.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace dts_viewer
{

struct MissionSoundsState
{
    std::vector<SoundHandle> voices;
    bool wind_active = false;
    SoundHandle wind_handle = kInvalidSound;
};

// Scan the scene for ambient sources and start them.  The `vol_dir`
// argument is the search root for ambient WAV files (e.g. base/Entities.vol);
// this v1 looks the files up in the same MaterialResolver pattern the
// renderer uses.  Returns the state owned by the caller (so a mission
// switch can unload it).
MissionSoundsState mission_sounds_load(
    const studio::content::mission::scene_graph& scene,
    const std::filesystem::path& vol_dir);

void mission_sounds_unload(MissionSoundsState& state);

} // namespace dts_viewer

#endif // DTS_VIEWER_MISSION_SOUNDS_HPP
