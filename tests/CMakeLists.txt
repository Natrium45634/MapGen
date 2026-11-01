# Catch2 v3: the system package if present, otherwise the pinned upstream tag.
find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
    include(FetchContent)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.4.0   # pinned, see docs/BUILD.md and THIRD_PARTY_LICENSES.md
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(Catch2)
endif()

file(GLOB MAPGEN_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
list(FILTER MAPGEN_TEST_SOURCES EXCLUDE REGEX "test_ui\\.cpp$")
add_executable(mapgen_tests ${MAPGEN_TEST_SOURCES} TestSupport.h TestSupport.cpp)
target_link_libraries(mapgen_tests PRIVATE mapgen_app mapgen_io mapgen_render Catch2::Catch2)
target_compile_definitions(mapgen_tests PRIVATE MAPGEN_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

# Fast unit and invariant tests (P0 gate), statistical batch tests and golden maps as separate CTest entries.
add_test(NAME unit COMMAND mapgen_tests "~[stat]~[golden]~[slow]")
add_test(NAME slow COMMAND mapgen_tests "[slow]")
add_test(NAME golden COMMAND mapgen_tests "[golden]")
add_test(NAME statistics COMMAND mapgen_tests "[stat]")
set_tests_properties(statistics PROPERTIES LABELS "stat" TIMEOUT 3600)
set_tests_properties(slow PROPERTIES TIMEOUT 1800)

if(MAPGEN_BUILD_GUI)
    add_executable(mapgen_ui_tests test_ui.cpp TestSupport.h TestSupport.cpp)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    target_link_libraries(mapgen_ui_tests PRIVATE mapgen_app mapgen_ui Catch2::Catch2 Qt6::Test)
    target_compile_definitions(mapgen_ui_tests PRIVATE MAPGEN_UI_TESTS=1)
    add_test(NAME ui COMMAND mapgen_ui_tests)
    set_tests_properties(ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
