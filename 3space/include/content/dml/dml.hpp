#ifndef OPEN_SIEGE_CONTENT_DML_DML_HPP
#define OPEN_SIEGE_CONTENT_DML_DML_HPP

// Discoverability wrapper for the Dynamix Material List (DML) format.
//
// DML is parsed in-tree by studio::content::dts::darkstar::read_material_list,
// which is dispatched by darkstar::read_shape() when the PERS class_name is
// "TS::MaterialList" (the same engine class used for both standalone .dml
// files and the inline material list trailing a .dts shape). This header
// exists so future readers searching for "dml" land on the correct entry
// points without having to know the parser lives under the dts/ namespace.
//
// Field-level metadata (flags, alpha, type, friction, elasticity,
// useDefaultProperties) is surfaced per-material by
// studio::content::dts::dts_renderable_shape::get_materials().

#include <istream>
#include <optional>
#include <variant>

#include "content/dts/darkstar.hpp"
#include "content/dts/darkstar_structures.hpp"

namespace studio::content::dml
{
  using studio::content::dts::darkstar::material_list_variant;

  // Returns true iff the next PERS object on the stream is a "TS::MaterialList".
  // Stream position is restored on return regardless of outcome.
  inline bool is_dml(std::istream& stream)
  {
    const auto start = stream.tellg();
    try
    {
      auto result = studio::content::dts::darkstar::read_shape(stream);
      stream.seekg(start, std::ios::beg);
      return std::get_if<material_list_variant>(&result) != nullptr;
    }
    catch (...)
    {
      stream.clear();
      stream.seekg(start, std::ios::beg);
      return false;
    }
  }

  // Returns the parsed material list if the stream holds a standalone DML,
  // std::nullopt otherwise. On success the stream is consumed up to the end
  // of the material list; on failure the stream is rewound to its starting
  // position.
  inline std::optional<material_list_variant> get_dml_data(std::istream& stream)
  {
    const auto start = stream.tellg();
    try
    {
      auto result = studio::content::dts::darkstar::read_shape(stream);
      if (auto* ml = std::get_if<material_list_variant>(&result))
      {
        return std::move(*ml);
      }
      stream.seekg(start, std::ios::beg);
      return std::nullopt;
    }
    catch (...)
    {
      stream.clear();
      stream.seekg(start, std::ios::beg);
      return std::nullopt;
    }
  }
}// namespace studio::content::dml

#endif//OPEN_SIEGE_CONTENT_DML_DML_HPP
