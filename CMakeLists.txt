cmake_minimum_required(VERSION 3.21)

project(MapGen
    VERSION 1.0.0
    DESCRIPTION "Генератор физико-географических карт землеподобной планеты по сиду"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MAPGEN_BUILD_GUI "Build the Qt desktop application" ON)
option(MAPGEN_BUILD_TESTS "Build automatic tests" ON)
option(MAPGEN_BUILD_BENCHMARKS "Build benchmarks" ON)

# Algorithm / schema versions: part of every RNG stream key and every saved world.
set(MAPGEN_ALGORITHM_VERSION 1)
set(MAPGEN_SCHEMA_VERSION 1)

include(cmake/CompilerOptions.cmake)

configure_file(core/Version.h.in ${CMAKE_BINARY_DIR}/generated/mapgen/Version.h @ONLY)

add_subdirectory(core)

# Qt is needed by io/render/ui/app; the numerical core is pure C++20.
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui)
if(MAPGEN_BUILD_GUI)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets)
endif()
set(CMAKE_AUTOMOC ON)

add_subdirectory(io)
add_subdirectory(render)
if(MAPGEN_BUILD_GUI)
    add_subdirectory(ui)
endif()
add_subdirectory(app)

if(MAPGEN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MAPGEN_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
