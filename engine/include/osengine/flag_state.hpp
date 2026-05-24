#ifndef OSENGINE_FLAG_STATE_HPP
#define OSENGINE_FLAG_STATE_HPP

// Spec 28/08 — CTF flag entities + capture logic.
//
// Two flags (Red + Blue) are placed at team flag stands extracted
// from the mission. Each tick:
//   * Active carriers' flag.position follows the carrier's player_state.pos.
//   * Alive players within kPickupRadius of an unattended flag of the
//     OPPOSITE team become its carrier.
//   * Alive players within kPickupRadius of an unattended-AND-dropped
//     flag of their OWN team return it home immediately.
//   * Dropped flags older than kAutoReturnMs return home.
//   * If the carrier brings the enemy flag within kPickupRadius of
//     their OWN team's flag stand AND that team's flag is currently
//     AtHome, a CaptureEvent is emitted and both flags reset to home.
//
// v1 limits: no flag-touch SFX, no jet-drain-while-carrying, no flag
// throw-on-keypress (carriers can't drop on demand). Polish in track 31.

#include <osengine/session_table.hpp>

#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

namespace dts_viewer
{

struct LoadedMission;

enum class FlagPhase : std::uint8_t {
    AtHome   = 0,
    Carried  = 1,
    Dropped  = 2,
};

struct Flag
{
    Team          team           = Team::Spectator;
    FlagPhase     phase          = FlagPhase::AtHome;
    glm::vec3     position       {0.0f};
    glm::vec3     home_position  {0.0f};
    std::uint16_t carrier_slot   = 0xFFFFu;
    std::uint64_t dropped_at_ms  = 0;
};

struct CaptureEvent
{
    std::uint16_t capturer_slot;
    Team          flag_taken;       // which flag's *team* was captured
};

class FlagWorld
{
public:
    // Pickup / capture radius in metres. Public so callers can match
    // the radius for ghost-render purposes.
    static constexpr float kPickupRadius   = 3.0f;
    static constexpr std::uint64_t kAutoReturnMs = 30'000;

    // Initialise from a mission. Walks the scene graph for marker
    // datablock names containing "FlagStand" + team affinity. If no
    // flag stands found, falls back to home_position from the
    // mission's spawn points (Red & Blue from spec 28/05's extract).
    void load_from_mission(const LoadedMission& mission);

    // Test-only seed: directly install two flags at known home positions.
    void seed_for_test(const glm::vec3& red_home,
                       const glm::vec3& blue_home);

    void tick(SessionTable& sessions,
              std::vector<CaptureEvent>& out_caps,
              std::uint64_t now_ms);

    // Called when a player dies (spec 28/07 KillEvent). If they were
    // carrying any flag, drop it at death_pos.
    void on_player_died(std::uint16_t slot,
                        const glm::vec3& death_pos,
                        std::uint64_t now_ms);

    const Flag* red()  const { return loaded_ ? &red_  : nullptr; }
    const Flag* blue() const { return loaded_ ? &blue_ : nullptr; }

    bool loaded() const { return loaded_; }

    static int selftest();

private:
    Flag red_;
    Flag blue_;
    bool loaded_ = false;

    Flag&       flag_for(Team t);
    const Flag& flag_for(Team t) const;
    void        reset_to_home(Flag& f);
};

} // namespace dts_viewer

#endif // OSENGINE_FLAG_STATE_HPP
