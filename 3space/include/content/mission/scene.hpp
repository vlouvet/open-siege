#ifndef DARKSTAR_CONTENT_MISSION_SCENE_HPP
#define DARKSTAR_CONTENT_MISSION_SCENE_HPP

// Scene-graph builder for Tribes 1 mission files.
//
// Lifts the untyped mis_file AST (from mis.hpp / read_mis_file) into
// a strongly-typed tree where each recognised MIS object class has a
// dedicated C++ struct.  Unknown classes are preserved as monostate
// nodes with their raw properties in extra_properties, so no data is
// lost during the transformation.
//
// Derived by inspection of real .mis files shipped with Tribes 1.41.
// No leaked Dynamix source was consulted.

#include "content/mission/mis.hpp"
#include <array>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace studio::content::mission {

  // ---------------------------------------------------------------------------
  // Transform (position + rotation)
  // ---------------------------------------------------------------------------

  // position: 3 floats (world-space XYZ).
  // rotation: 4 floats stored as [rx, ry, rz, w].
  //   In shipped .mis files rotation is always 3 Euler floats (XYZ).  The
  //   fourth element is set to 1.0 (identity w) when parsing a 3-float value.
  struct transform {
    std::array<float, 3> position{ 0.0f, 0.0f, 0.0f };
    std::array<float, 4> rotation{ 0.0f, 0.0f, 0.0f, 1.0f }; // xyz + w
  };

  // ---------------------------------------------------------------------------
  // datablock_ref — name-only reference to a datablock defined in .cs scripts.
  // Resolution is deferred to Track 16 (cscript-bindings).
  // ---------------------------------------------------------------------------
  struct datablock_ref {
    std::string name;
  };

  // ---------------------------------------------------------------------------
  // Typed node payloads
  // ---------------------------------------------------------------------------

  // StaticShape — placed object with a dataBlock
  struct node_static_shape {
    transform xf;
    datablock_ref data_block;
  };

  // Item — pickup / inventory item
  struct node_item {
    transform xf;
    datablock_ref data_block;
  };

  // InteriorShape — interior geometry (.dis file)
  struct node_interior {
    transform xf;
    std::string shape_name;    // fileName property (the .dis filename)
    std::string light_params;  // lightParams property (raw string)
  };

  // Turret — placed turret
  struct node_turret {
    transform xf;
    datablock_ref data_block;
  };

  // Sensor — placed sensor
  struct node_sensor {
    transform xf;
    datablock_ref data_block;
  };

  // Marker — drop-point / path / map marker; dataBlock discriminates type
  struct node_marker {
    transform xf;
    datablock_ref data_block;
  };

  // Moveable — animated object (elevators, doors)
  struct node_moveable {
    transform xf;
    datablock_ref data_block;
    std::string status;           // "up", "down", etc. — runtime save-state
    float close_time  = 0.0f;
    float delay_time  = 0.0f;
  };

  // SimPath — waypoint path container (children are Marker nodes)
  struct node_path {
    bool is_looping    = false;
    bool is_compressed = false;
  };

  // Trigger — axis-aligned or sphere trigger volume
  struct node_trigger {
    std::array<float, 6> bounding_box{};  // minX minY minZ maxX maxY maxZ
    bool is_sphere = false;
    std::string status;
  };

  // SimLight — dynamic or static scene light
  struct node_sim_light {
    transform xf;
    int type     = 0;
    std::array<float, 3> color{};
    float radius = 0.0f;   // stored as `range` in .mis files
  };

  // Planet — billboard sun/moon
  struct node_planet {
    transform xf;
    std::string texture;             // fileName property
    float radius = 0.0f;
    std::array<float, 3> intensity{};// intensity property (sun colour magnitude)
    std::array<float, 3> ambient{};  // ambient property (scene ambient term)
  };

  // Sky — sky dome; dml_name references a .dml material list
  struct node_sky {
    std::string dml_name;
    std::array<int, 16> textures{};
    std::array<float, 3> ambient_color{};
    std::array<float, 3> sky_color{};   // skyColor — dome top
    std::array<float, 3> haze_color{};  // hazeColor — dome bottom
  };

  // StarField — background star billboard layer
  struct node_star_field {
    std::array<std::array<float, 3>, 3> colors{};
    bool in_front_of_sky = false;
  };

  // Snowfall — particle precipitation
  struct node_snowfall {
    float intensity = 0.0f;
    std::array<float, 3> wind{};
    bool is_rain = false;
  };

  // SimTerrain — heightmap terrain
  struct node_sim_terrain {
    std::string ted_filename;   // tedFileName property
    float gravity = 0.0f;       // contGravity Z component
    float drag    = 0.0f;       // contDrag
    std::array<float, 4> fog{}; // visibleDistance, hazeDistance, hazeVerticalMin, hazeVerticalMax
  };

  // SimPalette — palette definition file
  struct node_sim_palette {
    std::string ppl_filename;   // fileName property
  };

  // MissionCenterPos — 2-D mission map bounds
  struct node_mission_center {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
  };

  // SimVolume — VOL archive to mount; captured in declaration order
  struct node_sim_volume {
    std::string file_name;   // fileName property
  };

  // TeamGroup — named team container
  struct node_team_group {
    std::string name;  // instance_name carried through
  };

  // ---------------------------------------------------------------------------
  // Variant covering every typed payload.
  // std::monostate = SimGroup containers and unknown class names.
  // ---------------------------------------------------------------------------
  using node_payload = std::variant<
    std::monostate,
    node_static_shape, node_item, node_interior, node_turret, node_sensor,
    node_marker, node_moveable, node_path, node_trigger,
    node_sim_light, node_planet, node_sky, node_star_field, node_snowfall,
    node_sim_terrain, node_sim_palette, node_mission_center, node_sim_volume,
    node_team_group>;

  // ---------------------------------------------------------------------------
  // scene_node — one vertex in the scene graph
  // ---------------------------------------------------------------------------
  struct scene_node {
    std::string class_name;                      // raw class name string
    std::optional<std::string> instance_name;
    node_payload payload;
    std::vector<scene_node> children;
    std::vector<mis_property> extra_properties;  // unconsumed properties
  };

  // ---------------------------------------------------------------------------
  // scene_graph — top-level result of build_scene()
  // ---------------------------------------------------------------------------
  struct scene_graph {
    scene_node root;                                // MissionGroup SimGroup
    std::vector<node_sim_volume> volumes_in_order;  // DFS declaration order
    const node_sim_terrain*    terrain = nullptr;   // points into root subtree
    const node_sky*            sky     = nullptr;
    const node_sim_palette*    palette = nullptr;
    const node_mission_center* center  = nullptr;
    mis_trailer trailer;
  };

  // Build a scene_graph from a fully-parsed mis_file.
  // The non-owning pointers inside scene_graph point into root's subtree —
  // do NOT move or copy root after construction.
  scene_graph build_scene(const mis_file& parsed);

}  // namespace studio::content::mission

#endif  // DARKSTAR_CONTENT_MISSION_SCENE_HPP
