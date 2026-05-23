#ifndef DTS_VIEWER_AI_PERCEPTION_HPP
#define DTS_VIEWER_AI_PERCEPTION_HPP

// Spec 18/04 — bot perception. Finds the closest hostile target with LOS
// for an AIPlayer. v1 perception is engine-side only — no script
// dispatch — so the tick loop calls these directly.

class AIPlayer;

namespace dts_viewer {

// Engine-side ray cast. v1 implementation: distance check against the
// per-tick position cache, no terrain occlusion yet. Returns true if the
// target is within `max_dist` metres. A later spec will integrate the
// terrain HeightSampler so ray vs height-field works.
bool has_los(const AIPlayer& bot, const AIPlayer& target, float max_dist = 250.0f);

// Pick the best target for `bot` from the global AIPlayer pool. Returns
// nullptr if no hostile is within range / has LOS.
AIPlayer* find_best_target(const AIPlayer& bot);

// Per-tick perception step: updates current target + LOS flag, fires the
// four script callbacks (Acquired / Lost / Regained / Died). Called from
// ai_tick_all for every AIPlayer with mAutoTargets == true.
void run_perception_tick(AIPlayer& bot);

} // namespace dts_viewer

#endif // DTS_VIEWER_AI_PERCEPTION_HPP
