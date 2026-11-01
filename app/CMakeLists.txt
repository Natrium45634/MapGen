# Shared app code (self-test) + the two executables.
add_library(mapgen_app STATIC SelfTest.cpp SelfTest.h)
target_link_libraries(mapgen_app PUBLIC mapgen_core mapgen_io Qt6::Core Qt6::Gui)

add_executable(mapgen-cli main_cli.cpp)
target_link_libraries(mapgen-cli PRIVATE mapgen_app)

if(MAPGEN_BUILD_GUI)
    set(MAPGEN_GUI_SOURCES main_gui.cpp)
    if(WIN32)
        configure_file(${PROJECT_SOURCE_DIR}/packaging/MapGen.rc.in ${CMAKE_CURRENT_BINARY_DIR}/MapGen.rc @ONLY)
        list(APPEND MAPGEN_GUI_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/MapGen.rc)
    endif()
    add_executable(MapGen WIN32 ${MAPGEN_GUI_SOURCES})
    target_link_libraries(MapGen PRIVATE mapgen_app mapgen_ui Qt6::Widgets)
    set_target_properties(MapGen PROPERTIES OUTPUT_NAME "MapGen")
endif()

if(WIN32)
    target_sources(mapgen-cli PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/MapGen.rc)
    if(NOT MAPGEN_BUILD_GUI)
        configure_file(${PROJECT_SOURCE_DIR}/packaging/MapGen.rc.in ${CMAKE_CURRENT_BINARY_DIR}/MapGen.rc @ONLY)
    endif()
endif()

install(TARGETS mapgen-cli RUNTIME DESTINATION .)
if(MAPGEN_BUILD_GUI)
    install(TARGETS MapGen RUNTIME DESTINATION .)
endif()
