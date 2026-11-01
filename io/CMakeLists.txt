# Files and exports: .geoworld container, PNG / 16-bit heightmap / GeoTIFF / GeoJSON / CSV / JSON.
file(GLOB MAPGEN_IO_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
add_library(mapgen_io STATIC ${MAPGEN_IO_SOURCES})
target_link_libraries(mapgen_io PUBLIC mapgen_core mapgen_render Qt6::Core Qt6::Gui)
