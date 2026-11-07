cmake_minimum_required(VERSION 3.20)

# CMake initializes Apple deployment defaults during the first project() call.
# Explicit settings and toolchains retain ownership of their target platform.
if (CMAKE_HOST_APPLE AND (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        AND NOT CMAKE_TOOLCHAIN_FILE AND "$ENV{CMAKE_TOOLCHAIN_FILE}" STREQUAL ""
        AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment target")
endif ()

project(example_live2d VERSION 0.1.0 LANGUAGES NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/HuxerUIProject.cmake")
if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

file(GLOB_RECURSE APP_SOURCE_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)
if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    if (WIN32)
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/main.cpp")
    elseif (APPLE AND NOT IOS)
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/main.cpp")
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/main.cpp")
    endif ()
endif ()

huxerui_add_app(example_live2d
        SOURCES
            ${APP_SOURCE_FILES}
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            app
        ${HUXERUI_APP_RESOURCE_OUTPUT_ARGUMENTS}
        BUNDLE_NAME
            "Live2D Preview"
        BUNDLE_IDENTIFIER
            "org.huxerui.lib.live2d.preview"
)

huxerui_configure_project_app(example_live2d)

huxerui_use_library(example_live2d
        TARGET HuxerUI::Live2D
        PATH "${CMAKE_CURRENT_SOURCE_DIR}/../.."
)

if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    if (APPLE OR EMSCRIPTEN)
        live2d_configure_app(example_live2d)
    endif ()
endif ()

option(LIVE2D_BUILD_INTEGRATION_TEST "Build the host integration test" OFF)
if (LIVE2D_BUILD_INTEGRATION_TEST AND APPLE AND NOT IOS AND NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    find_package(HuxerUI CONFIG REQUIRED COMPONENTS static testing)
    add_executable(live2d_integration MACOSX_BUNDLE "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/model_integration.cpp")
    target_link_libraries(live2d_integration PRIVATE HuxerUI::Live2D HuxerUI::testing)
    target_compile_definitions(live2d_integration PRIVATE
            LIVE2D_TEST_MODEL_ROOT="${LIVE2D_CUBISM_NATIVE_ROOT}/Samples/Resources/Wanko")
    huxerui_enable_codegen(live2d_integration)
    live2d_configure_app(live2d_integration)
endif ()
