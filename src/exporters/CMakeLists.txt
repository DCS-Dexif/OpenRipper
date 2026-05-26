# Exporters: format writers for captured meshes / textures.
# Linked into every backend.

add_library(openripper_exporters STATIC
    obj_exporter.cpp
    dds_exporter.cpp
    png_exporter.cpp
    material_exporter.cpp
    session_exporter.cpp
)

target_link_libraries(openripper_exporters
    PUBLIC  openripper_core
    PRIVATE stb_image_write
)

openripper_apply_common_flags(openripper_exporters)
