// Scene-graph builder for Tribes 1 mission files.
//
// Walks the untyped mis_file AST and produces a strongly-typed scene_graph.
// Class-name matching is case-insensitive to handle the occasional lower-case
// `simGroup` found in shipped .mis files.
//
// Format derived by ASCII/hex inspection of the 42 .mis files shipped with
// Tribes 1.41 freeware.  No leaked Dynamix source was consulted.

#include "content/mission/scene.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <string>

namespace studio::content::mission
{

  // ---------------------------------------------------------------------------
  // Internal helpers
  // ---------------------------------------------------------------------------

  namespace
  {
    // Case-insensitive string comparison.
    bool iequal(const std::string& a, const char* b)
    {
      std::size_t n = std::strlen(b);
      if (a.size() != n) return false;
      for (std::size_t i = 0; i < n; ++i)
      {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
          return false;
      }
      return true;
    }

    // Find a property by key (first match; case-sensitive key comparison
    // as shipped .mis files use consistent casing inside an object).
    const mis_property* find_prop(
      const std::vector<mis_property>& props,
      const std::string& key)
    {
      for (const auto& p : props)
        if (p.key == key) return &p;
      return nullptr;
    }

    // Mark a key as consumed by erasing it from the leftover set.
    void consume(std::vector<bool>& consumed,
                 const std::vector<mis_property>& props,
                 const std::string& key)
    {
      for (std::size_t i = 0; i < props.size(); ++i)
        if (props[i].key == key) consumed[i] = true;
    }

    // Consume an array key (all indices).
    void consume_array(std::vector<bool>& consumed,
                       const std::vector<mis_property>& props,
                       const std::string& key)
    {
      for (std::size_t i = 0; i < props.size(); ++i)
        if (props[i].key == key && props[i].array_index.has_value())
          consumed[i] = true;
    }

    // Parse a transform from position / rotation properties.
    transform parse_transform(const std::vector<mis_property>& props,
                              std::vector<bool>& consumed)
    {
      transform xf;
      if (const auto* p = find_prop(props, "position"))
      {
        auto v = parse_vec3(p->value);
        if (v) xf.position = *v;
        consume(consumed, props, "position");
      }
      if (const auto* p = find_prop(props, "rotation"))
      {
        // Rotation is stored as 3 Euler floats in every shipped .mis file.
        // The fourth (w) element defaults to 1.0 (identity).
        auto v = parse_vec3(p->value);
        if (v)
        {
          xf.rotation[0] = (*v)[0];
          xf.rotation[1] = (*v)[1];
          xf.rotation[2] = (*v)[2];
          xf.rotation[3] = 1.0f;
        }
        consume(consumed, props, "rotation");
      }
      return xf;
    }

    // Parse a datablock reference (dataBlock property).
    datablock_ref parse_datablock(const std::vector<mis_property>& props,
                                  std::vector<bool>& consumed)
    {
      datablock_ref db;
      if (const auto* p = find_prop(props, "dataBlock"))
      {
        db.name = p->value;
        consume(consumed, props, "dataBlock");
      }
      return db;
    }

    // Build unconsumed-property list.
    std::vector<mis_property> make_extra(
      const std::vector<mis_property>& props,
      const std::vector<bool>& consumed)
    {
      std::vector<mis_property> out;
      for (std::size_t i = 0; i < props.size(); ++i)
        if (!consumed[i]) out.push_back(props[i]);
      return out;
    }

    // ---------------------------------------------------------------------------
    // Per-class payload builders
    // ---------------------------------------------------------------------------

    node_payload build_static_shape(const std::vector<mis_property>& props,
                                    std::vector<bool>& consumed)
    {
      node_static_shape n;
      n.xf         = parse_transform(props, consumed);
      n.data_block = parse_datablock(props, consumed);
      return n;
    }

    node_payload build_item(const std::vector<mis_property>& props,
                            std::vector<bool>& consumed)
    {
      node_item n;
      n.xf         = parse_transform(props, consumed);
      n.data_block = parse_datablock(props, consumed);
      return n;
    }

    node_payload build_interior(const std::vector<mis_property>& props,
                                std::vector<bool>& consumed)
    {
      node_interior n;
      n.xf = parse_transform(props, consumed);
      if (const auto* p = find_prop(props, "fileName"))
      {
        n.shape_name = p->value;
        consume(consumed, props, "fileName");
      }
      if (const auto* p = find_prop(props, "lightParams"))
      {
        n.light_params = p->value;
        consume(consumed, props, "lightParams");
      }
      return n;
    }

    node_payload build_turret(const std::vector<mis_property>& props,
                              std::vector<bool>& consumed)
    {
      node_turret n;
      n.xf         = parse_transform(props, consumed);
      n.data_block = parse_datablock(props, consumed);
      return n;
    }

    node_payload build_sensor(const std::vector<mis_property>& props,
                              std::vector<bool>& consumed)
    {
      node_sensor n;
      n.xf         = parse_transform(props, consumed);
      n.data_block = parse_datablock(props, consumed);
      return n;
    }

    node_payload build_marker(const std::vector<mis_property>& props,
                              std::vector<bool>& consumed)
    {
      node_marker n;
      n.xf         = parse_transform(props, consumed);
      n.data_block = parse_datablock(props, consumed);
      return n;
    }

    node_payload build_moveable(const std::vector<mis_property>& props,
                                std::vector<bool>& consumed)
    {
      node_moveable n;
      n.xf         = parse_transform(props, consumed);
      n.data_block = parse_datablock(props, consumed);
      // Status — stored as "Status" (capital S in shipped files)
      if (const auto* p = find_prop(props, "Status"))
      {
        n.status = p->value;
        consume(consumed, props, "Status");
      }
      else if (const auto* p2 = find_prop(props, "status"))
      {
        n.status = p2->value;
        consume(consumed, props, "status");
      }
      if (const auto* p = find_prop(props, "closeTime"))
      {
        auto v = parse_float(p->value);
        if (v) n.close_time = *v;
        consume(consumed, props, "closeTime");
      }
      if (const auto* p = find_prop(props, "delayTime"))
      {
        auto v = parse_float(p->value);
        if (v) n.delay_time = *v;
        consume(consumed, props, "delayTime");
      }
      return n;
    }

    node_payload build_path(const std::vector<mis_property>& props,
                            std::vector<bool>& consumed)
    {
      node_path n;
      if (const auto* p = find_prop(props, "isLooping"))
      {
        auto v = parse_bool(p->value);
        if (v) n.is_looping = *v;
        consume(consumed, props, "isLooping");
      }
      if (const auto* p = find_prop(props, "isCompressed"))
      {
        auto v = parse_bool(p->value);
        if (v) n.is_compressed = *v;
        consume(consumed, props, "isCompressed");
      }
      return n;
    }

    node_payload build_trigger(const std::vector<mis_property>& props,
                               std::vector<bool>& consumed)
    {
      node_trigger n;
      // position/rotation exist on Trigger but are extra (not in struct)
      // still consume them so they don't leak into extra_properties
      parse_transform(props, consumed);  // consumes position + rotation
      if (const auto* p = find_prop(props, "boundingBox"))
      {
        auto v = parse_vec6(p->value);
        if (v) n.bounding_box = *v;
        consume(consumed, props, "boundingBox");
      }
      if (const auto* p = find_prop(props, "isSphere"))
      {
        auto v = parse_bool(p->value);
        if (v) n.is_sphere = *v;
        consume(consumed, props, "isSphere");
      }
      if (const auto* p = find_prop(props, "status"))
      {
        n.status = p->value;
        consume(consumed, props, "status");
      }
      // dataBlock is common on Trigger; consume it (not in struct)
      consume(consumed, props, "dataBlock");
      return n;
    }

    node_payload build_sim_light(const std::vector<mis_property>& props,
                                 std::vector<bool>& consumed)
    {
      node_sim_light n;
      n.xf = parse_transform(props, consumed);
      if (const auto* p = find_prop(props, "type"))
      {
        // strtol tolerates leading whitespace
        char* endp = nullptr;
        long v = std::strtol(p->value.c_str(), &endp, 10);
        if (endp != p->value.c_str()) n.type = static_cast<int>(v);
        consume(consumed, props, "type");
      }
      // color property — "r g b"
      if (const auto* p = find_prop(props, "color"))
      {
        auto v = parse_vec3(p->value);
        if (v) n.color = *v;
        consume(consumed, props, "color");
      }
      // range
      if (const auto* p = find_prop(props, "range"))
      {
        auto v = parse_float(p->value);
        if (v) n.radius = *v;
        consume(consumed, props, "range");
      }
      return n;
    }

    node_payload build_planet(const std::vector<mis_property>& props,
                              std::vector<bool>& consumed)
    {
      node_planet n;
      n.xf = parse_transform(props, consumed);
      if (const auto* p = find_prop(props, "fileName"))
      {
        n.texture = p->value;
        consume(consumed, props, "fileName");
      }
      if (const auto* p = find_prop(props, "size"))
      {
        auto v = parse_float(p->value);
        if (v) n.radius = *v;
        consume(consumed, props, "size");
      }
      if (const auto* p = find_prop(props, "intensity"))
      {
        auto v = parse_vec3(p->value);
        if (v) n.intensity = *v;
        consume(consumed, props, "intensity");
      }
      if (const auto* p = find_prop(props, "ambient"))
      {
        auto v = parse_vec3(p->value);
        if (v) n.ambient = *v;
        consume(consumed, props, "ambient");
      }
      return n;
    }

    node_payload build_sky(const std::vector<mis_property>& props,
                           std::vector<bool>& consumed)
    {
      node_sky n;
      if (const auto* p = find_prop(props, "dmlName"))
      {
        n.dml_name = p->value;
        consume(consumed, props, "dmlName");
      }
      // textures[0..15]
      for (const auto& prop : props)
      {
        if (prop.key == "textures" && prop.array_index.has_value())
        {
          std::int32_t idx = *prop.array_index;
          if (idx >= 0 && idx < 16)
          {
            char* endp = nullptr;
            long v = std::strtol(prop.value.c_str(), &endp, 10);
            if (endp != prop.value.c_str())
              n.textures[static_cast<std::size_t>(idx)] = static_cast<int>(v);
          }
        }
      }
      consume_array(consumed, props, "textures");
      // ambientColor (not present in every shipped sky — ignore if missing)
      if (const auto* p = find_prop(props, "ambientColor"))
      {
        auto v = parse_vec3(p->value);
        if (v) n.ambient_color = *v;
        consume(consumed, props, "ambientColor");
      }
      if (const auto* p = find_prop(props, "skyColor"))
      {
        auto v = parse_vec3(p->value);
        if (v) n.sky_color = *v;
        consume(consumed, props, "skyColor");
      }
      if (const auto* p = find_prop(props, "hazeColor"))
      {
        auto v = parse_vec3(p->value);
        if (v) n.haze_color = *v;
        consume(consumed, props, "hazeColor");
      }
      return n;
    }

    node_payload build_star_field(const std::vector<mis_property>& props,
                                  std::vector<bool>& consumed)
    {
      node_star_field n;
      if (const auto* p = find_prop(props, "inFrontOfSky"))
      {
        auto v = parse_bool(p->value);
        if (v) n.in_front_of_sky = *v;
        consume(consumed, props, "inFrontOfSky");
      }
      // colors[0..2]
      for (const auto& prop : props)
      {
        if (prop.key == "colors" && prop.array_index.has_value())
        {
          std::int32_t idx = *prop.array_index;
          if (idx >= 0 && idx < 3)
          {
            auto v = parse_vec3(prop.value);
            if (v)
              n.colors[static_cast<std::size_t>(idx)] = *v;
          }
        }
      }
      consume_array(consumed, props, "colors");
      return n;
    }

    node_payload build_snowfall(const std::vector<mis_property>& props,
                                std::vector<bool>& consumed)
    {
      node_snowfall n;
      if (const auto* p = find_prop(props, "intensity"))
      {
        auto v = parse_float(p->value);
        if (v) n.intensity = *v;
        consume(consumed, props, "intensity");
      }
      if (const auto* p = find_prop(props, "wind"))
      {
        auto v = parse_vec3(p->value);
        if (v) n.wind = *v;
        consume(consumed, props, "wind");
      }
      // property name in shipped files is `rain` (not `isRain`)
      if (const auto* p = find_prop(props, "rain"))
      {
        auto v = parse_bool(p->value);
        if (v) n.is_rain = *v;
        consume(consumed, props, "rain");
      }
      else if (const auto* p2 = find_prop(props, "isRain"))
      {
        auto v = parse_bool(p2->value);
        if (v) n.is_rain = *v;
        consume(consumed, props, "isRain");
      }
      return n;
    }

    node_payload build_sim_terrain(const std::vector<mis_property>& props,
                                   std::vector<bool>& consumed)
    {
      node_sim_terrain n;
      if (const auto* p = find_prop(props, "tedFileName"))
      {
        n.ted_filename = p->value;
        consume(consumed, props, "tedFileName");
      }
      // contGravity is a vec3; Z component is stored in n.gravity
      if (const auto* p = find_prop(props, "contGravity"))
      {
        auto v = parse_vec3(p->value);
        if (v) n.gravity = (*v)[2];
        consume(consumed, props, "contGravity");
      }
      if (const auto* p = find_prop(props, "contDrag"))
      {
        auto v = parse_float(p->value);
        if (v) n.drag = *v;
        consume(consumed, props, "contDrag");
      }
      // fog: [visibleDistance, hazeDistance, hazeVerticalMin, hazeVerticalMax]
      if (const auto* p = find_prop(props, "visibleDistance"))
      {
        auto v = parse_float(p->value);
        if (v) n.fog[0] = *v;
        consume(consumed, props, "visibleDistance");
      }
      if (const auto* p = find_prop(props, "hazeDistance"))
      {
        auto v = parse_float(p->value);
        if (v) n.fog[1] = *v;
        consume(consumed, props, "hazeDistance");
      }
      if (const auto* p = find_prop(props, "hazeVerticalMin"))
      {
        auto v = parse_float(p->value);
        if (v) n.fog[2] = *v;
        consume(consumed, props, "hazeVerticalMin");
      }
      if (const auto* p = find_prop(props, "hazeVerticalMax"))
      {
        auto v = parse_float(p->value);
        if (v) n.fog[3] = *v;
        consume(consumed, props, "hazeVerticalMax");
      }
      return n;
    }

    node_payload build_sim_palette(const std::vector<mis_property>& props,
                                   std::vector<bool>& consumed)
    {
      node_sim_palette n;
      if (const auto* p = find_prop(props, "fileName"))
      {
        n.ppl_filename = p->value;
        consume(consumed, props, "fileName");
      }
      return n;
    }

    node_payload build_mission_center(const std::vector<mis_property>& props,
                                      std::vector<bool>& consumed)
    {
      node_mission_center n;
      if (const auto* p = find_prop(props, "x"))
      {
        auto v = parse_float(p->value);
        if (v) n.x = *v;
        consume(consumed, props, "x");
      }
      if (const auto* p = find_prop(props, "y"))
      {
        auto v = parse_float(p->value);
        if (v) n.y = *v;
        consume(consumed, props, "y");
      }
      if (const auto* p = find_prop(props, "w"))
      {
        auto v = parse_float(p->value);
        if (v) n.w = *v;
        consume(consumed, props, "w");
      }
      if (const auto* p = find_prop(props, "h"))
      {
        auto v = parse_float(p->value);
        if (v) n.h = *v;
        consume(consumed, props, "h");
      }
      return n;
    }

    node_payload build_sim_volume(const std::vector<mis_property>& props,
                                  std::vector<bool>& consumed)
    {
      node_sim_volume n;
      if (const auto* p = find_prop(props, "fileName"))
      {
        n.file_name = p->value;
        consume(consumed, props, "fileName");
      }
      return n;
    }

    node_payload build_team_group(const mis_object& obj,
                                  std::vector<bool>& /*consumed*/)
    {
      node_team_group n;
      if (obj.instance_name.has_value())
        n.name = *obj.instance_name;
      return n;
    }

    // ---------------------------------------------------------------------------
    // Recursive node builder
    // ---------------------------------------------------------------------------

    // Forward declaration.
    scene_node build_node(const mis_object& obj,
                          std::vector<node_sim_volume>& volumes_out,
                          const node_sim_terrain*& terrain_out,
                          const node_sky*& sky_out,
                          const node_sim_palette*& palette_out,
                          const node_mission_center*& center_out);

    scene_node build_node(const mis_object& obj,
                          std::vector<node_sim_volume>& volumes_out,
                          const node_sim_terrain*& terrain_out,
                          const node_sky*& sky_out,
                          const node_sim_palette*& palette_out,
                          const node_mission_center*& center_out)
    {
      scene_node node;
      node.class_name    = obj.class_name;
      node.instance_name = obj.instance_name;

      // consumed[i] == true means props[i] was parsed into a typed field.
      std::vector<bool> consumed(obj.properties.size(), false);

      // ------------------------------------------------------------------
      // Dispatch on class name (case-insensitive)
      // ------------------------------------------------------------------
      const std::string& cn = obj.class_name;

      if (iequal(cn, "SimGroup")
       || iequal(cn, "MissionGroup")
       || iequal(cn, "DropPoints"))
      {
        node.payload = std::monostate{};
        // No properties consumed — all go to extra.
      }
      else if (iequal(cn, "StaticShape"))
      {
        node.payload = build_static_shape(obj.properties, consumed);
      }
      else if (iequal(cn, "Item"))
      {
        node.payload = build_item(obj.properties, consumed);
      }
      else if (iequal(cn, "InteriorShape"))
      {
        node.payload = build_interior(obj.properties, consumed);
      }
      else if (iequal(cn, "Turret"))
      {
        node.payload = build_turret(obj.properties, consumed);
      }
      else if (iequal(cn, "Sensor"))
      {
        node.payload = build_sensor(obj.properties, consumed);
      }
      else if (iequal(cn, "Marker"))
      {
        node.payload = build_marker(obj.properties, consumed);
      }
      else if (iequal(cn, "Moveable"))
      {
        node.payload = build_moveable(obj.properties, consumed);
      }
      else if (iequal(cn, "SimPath"))
      {
        node.payload = build_path(obj.properties, consumed);
      }
      else if (iequal(cn, "Trigger"))
      {
        node.payload = build_trigger(obj.properties, consumed);
      }
      else if (iequal(cn, "SimLight"))
      {
        node.payload = build_sim_light(obj.properties, consumed);
      }
      else if (iequal(cn, "Planet"))
      {
        node.payload = build_planet(obj.properties, consumed);
      }
      else if (iequal(cn, "Sky"))
      {
        node.payload = build_sky(obj.properties, consumed);
      }
      else if (iequal(cn, "StarField"))
      {
        node.payload = build_star_field(obj.properties, consumed);
      }
      else if (iequal(cn, "Snowfall"))
      {
        node.payload = build_snowfall(obj.properties, consumed);
      }
      else if (iequal(cn, "SimTerrain"))
      {
        node.payload = build_sim_terrain(obj.properties, consumed);
      }
      else if (iequal(cn, "SimPalette"))
      {
        node.payload = build_sim_palette(obj.properties, consumed);
      }
      else if (iequal(cn, "MissionCenterPos"))
      {
        node.payload = build_mission_center(obj.properties, consumed);
      }
      else if (iequal(cn, "SimVolume"))
      {
        node.payload = build_sim_volume(obj.properties, consumed);
      }
      else if (iequal(cn, "TeamGroup"))
      {
        node.payload = build_team_group(obj, consumed);
      }
      else
      {
        // Unknown class — preserve all properties in extra_properties.
        node.payload = std::monostate{};
      }

      // ------------------------------------------------------------------
      // Build extra_properties from unconsumed props.
      // ------------------------------------------------------------------
      node.extra_properties = make_extra(obj.properties, consumed);

      // ------------------------------------------------------------------
      // Recurse into children.
      // ------------------------------------------------------------------
      for (const auto& child : obj.children)
      {
        node.children.push_back(
          build_node(child, volumes_out, terrain_out, sky_out,
                     palette_out, center_out));
      }

      // ------------------------------------------------------------------
      // Register singletons / volume list (after building the node so that
      // the pointer we capture points into the final child vector slot).
      // ------------------------------------------------------------------
      if (std::holds_alternative<node_sim_volume>(node.payload))
      {
        volumes_out.push_back(std::get<node_sim_volume>(node.payload));
      }
      if (!terrain_out && std::holds_alternative<node_sim_terrain>(node.payload))
      {
        // We cannot safely take a pointer here because the node lives on
        // the stack; terrain_out is set by the caller after tree insertion.
        // Mark with a sentinel — see post-processing in build_scene().
        // (We use a two-pass approach: build tree, then DFS for pointers.)
      }

      return node;
    }

    // DFS over a built tree to find the first occurrence of each singleton
    // payload type and return raw pointers into the tree.
    void find_singletons(scene_node& node,
                         const node_sim_terrain*& terrain,
                         const node_sky*& sky,
                         const node_sim_palette*& palette,
                         const node_mission_center*& center)
    {
      if (!terrain && std::holds_alternative<node_sim_terrain>(node.payload))
        terrain = &std::get<node_sim_terrain>(node.payload);
      if (!sky && std::holds_alternative<node_sky>(node.payload))
        sky = &std::get<node_sky>(node.payload);
      if (!palette && std::holds_alternative<node_sim_palette>(node.payload))
        palette = &std::get<node_sim_palette>(node.payload);
      if (!center && std::holds_alternative<node_mission_center>(node.payload))
        center = &std::get<node_mission_center>(node.payload);

      for (auto& child : node.children)
        find_singletons(child, terrain, sky, palette, center);
    }

  }  // anonymous namespace

  // ---------------------------------------------------------------------------
  // Public API
  // ---------------------------------------------------------------------------

  scene_graph build_scene(const mis_file& parsed)
  {
    scene_graph sg;
    sg.trailer = parsed.trailer;

    // First pass: build the tree and collect volumes_in_order.
    const node_sim_terrain*    terrain_unused = nullptr;
    const node_sky*            sky_unused     = nullptr;
    const node_sim_palette*    palette_unused = nullptr;
    const node_mission_center* center_unused  = nullptr;

    sg.root = build_node(parsed.root,
                         sg.volumes_in_order,
                         terrain_unused, sky_unused,
                         palette_unused, center_unused);

    // Second pass: DFS over the stable tree to fix non-owning pointers.
    find_singletons(sg.root, sg.terrain, sg.sky, sg.palette, sg.center);

    return sg;
  }

}  // namespace studio::content::mission
