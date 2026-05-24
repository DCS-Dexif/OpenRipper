// OpenRipper - src/exporters/obj_exporter.hpp
//
// Wavefront OBJ writer. Emits positions, normals, and UV channels when the
// MeshSnapshot contains the corresponding semantic attributes.

#pragma once

#include "core/types.hpp"

#include <filesystem>

namespace openripper::exporters {

// Write `mesh` to `out_path` as an .obj file. Returns true on success.
// The exporter is conservative: if no Position attribute is found or the
// position format is unsupported, it logs a warning and returns false rather
// than emitting a corrupt file.
//
// Normals (vn) and UVs (vt) are emitted when the snapshot contains
// VertexSemantic::Normal and VertexSemantic::TexCoord attributes respectively.
// Face lines use the v/vt/vn form, omitting whichever channels are absent.
bool write_obj(const MeshSnapshot& mesh,
               const std::filesystem::path& out_path);

} // namespace openripper::exporters
