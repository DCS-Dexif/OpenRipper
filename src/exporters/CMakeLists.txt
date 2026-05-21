# Exporters: format writers for captured meshes / textures.
# Linked into every backend.

add_library(openripper_exporters STATIC
    obj_exporter.cpp
)

target_link_libraries(openripper_exporters
    PUBLIC openripper_core
)

openripper_apply_common_flags(openripper_exporters)
