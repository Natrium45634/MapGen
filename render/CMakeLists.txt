# Layer colourisation, palettes and legends (CPU side, used by the GUI, exports and tests).
file(GLOB MAPGEN_RENDER_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
add_library(mapgen_render STATIC ${MAPGEN_RENDER_SOURCES})
target_link_libraries(mapgen_render PUBLIC mapgen_core Qt6::Core Qt6::Gui)
