# Compiler settings shared by all targets.
#
# Determinism: no fast-math, no FMA contraction (the same source must give the
# same bits on the same platform regardless of how the optimiser fuses
# multiply-adds), precise floating point model on MSVC.

add_library(mapgen_options INTERFACE)

if(MSVC)
    target_compile_options(mapgen_options INTERFACE
        /W4 /permissive- /utf-8 /fp:precise /bigobj /EHsc /Zc:__cplusplus
        /wd4100 /wd4127 /wd4244 /wd4267 /wd4305 /wd4458 /wd4459 /wd4996)
    target_compile_definitions(mapgen_options INTERFACE
        NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES UNICODE _UNICODE)
else()
    target_compile_options(mapgen_options INTERFACE
        -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
        -ffp-contract=off -fno-fast-math)
endif()

target_include_directories(mapgen_options INTERFACE
    ${PROJECT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/generated)

find_package(Threads REQUIRED)
target_link_libraries(mapgen_options INTERFACE Threads::Threads)
