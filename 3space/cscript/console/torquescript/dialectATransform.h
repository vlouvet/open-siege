// Open Siege spec 16/03 — Tribes-1 dialect-A datablock syntax transform.
//
// Tribes-1 scripts declare datablocks as:
//   BulletData ChaingunBullet
//   {
//       mass = 0.05;
//       ...
//   };
//
// Torque3D's parser (which our cscript fork uses) only recognises:
//   datablock BulletData(ChaingunBullet) { mass = 0.05; ... };
//
// `transformTribes1Datablocks()` rewrites the former into the latter
// while preserving line/column positions inside the slot body (so error
// messages stay accurate). Strings and `// /* */` comments are skipped
// so we never touch text inside literals.
//
// The transform is idempotent — already-Torque-shaped sources pass
// through unchanged.

#ifndef _DIALECT_A_TRANSFORM_H_
#define _DIALECT_A_TRANSFORM_H_

#include <string>

namespace studio { namespace content { namespace cscript {

/// Rewrite Tribes-1 dialect-A datablock declarations into Torque3D
/// `datablock TypeName(Name) {...};` form. Safe on Torque-shaped input.
///
/// Recognises typenames whose identifier ends in "Data", "Body", or
/// "Image" — matches every datablock subclass present in the T1
/// scripts.vol corpus (BulletData, PlayerData, RocketData, ItemImage,
/// PlayerBody, etc.).
std::string transformTribes1Datablocks(const std::string& source);

}}} // namespace studio::content::cscript

#endif
