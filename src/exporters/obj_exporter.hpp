// OpenRipper - src/exporters/obj_exporter.hpp
//
// Wavefront OBJ writer. Stage 1 emits positions + faces only; Stage 2 will
// extend this with normals, UV channels and a sibling .mtl file once the
// backend starts producing fully-populated MeshSnapshots.

#pragma once

#include "core/types.hpp"

#include <filesystem>

namespace openripper::exporters {

// Write `mesh` to `out_path` as an .obj. Returns true on success. The
// exporter is conservative: if it cannot locate a Position attribute or the
// format is outside its supported subset, it logs a warning and returns
// false rather than emitting a corrupt file.
bool write_obj(const MeshSnapshot& mesh,
               const std::filesystem::path& out_path);

} // namespace openripper::exporters
