# Qt Widgets shell: main window, map views (OpenGL 3.3 core + software fallback), legends, info card.
file(GLOB MAPGEN_UI_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
add_library(mapgen_ui STATIC ${MAPGEN_UI_SOURCES})
target_link_libraries(mapgen_ui PUBLIC mapgen_core mapgen_render mapgen_io
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)
