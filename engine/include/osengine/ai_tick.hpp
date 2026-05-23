#ifndef DTS_VIEWER_AI_TICK_HPP
#define DTS_VIEWER_AI_TICK_HPP

// Spec 18/03 — per-frame AI tick. Walks every AIPlayer toward its current
// waypoint (per pathType), fires periodic callbacks, and (spec 18/04)
// runs perception. Cheap on average — bots that have no directives just
// no-op.

namespace dts_viewer {

// Call once per frame from the main game loop.
void ai_tick_all(float dt_seconds);

}

#endif // DTS_VIEWER_AI_TICK_HPP
