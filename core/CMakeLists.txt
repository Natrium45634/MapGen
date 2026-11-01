# Numerical engine: pure C++20, no Qt. Everything that defines the world lives here.
file(GLOB_RECURSE MAPGEN_CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/*.h)

add_library(mapgen_core STATIC ${MAPGEN_CORE_SOURCES})
target_link_libraries(mapgen_core PUBLIC mapgen_options)
