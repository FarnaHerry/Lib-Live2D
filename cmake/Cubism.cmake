set(LIVE2D_CUBISM_NATIVE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.cache/cubism/CubismSdkForNative-5-r.5"
        CACHE PATH "Cubism Native 5 R5 SDK root")
set(LIVE2D_CUBISM_WEB_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.cache/cubism/CubismSdkForWeb-5-r.5"
        CACHE PATH "Cubism Web 5 R5 SDK root")

if (EMSCRIPTEN)
    include("${CMAKE_CURRENT_LIST_DIR}/Web.cmake")
    return()
endif ()

if (NOT EXISTS "${LIVE2D_CUBISM_NATIVE_ROOT}/Core/include/Live2DCubismCore.h")
    message(FATAL_ERROR "Run python3 tools/fetch_cubism.py or set LIVE2D_CUBISM_NATIVE_ROOT")
endif ()

set(_live2d_core_dir "${LIVE2D_CUBISM_NATIVE_ROOT}/Core/lib")
if (ANDROID)
    set(_live2d_core "${_live2d_core_dir}/android/${ANDROID_ABI}/libLive2DCubismCore.a")
    set(FRAMEWORK_SOURCE OpenGL)
    set(_live2d_definitions CSM_TARGET_ANDROID_ES2)
elseif (APPLE)
    enable_language(OBJCXX)
    string(TOLOWER "${CMAKE_OSX_SYSROOT}" _live2d_apple_sysroot)
    if (CMAKE_OSX_ARCHITECTURES)
        set(_live2d_arch "${CMAKE_OSX_ARCHITECTURES}")
    else ()
        set(_live2d_arch "${CMAKE_SYSTEM_PROCESSOR}")
    endif ()
    if (IOS)
        if (_live2d_apple_sysroot MATCHES "simulator")
            set(_live2d_core "${_live2d_core_dir}/ios/Release-iphonesimulator-${_live2d_arch}/libLive2DCubismCore.a")
        else ()
            set(_live2d_core "${_live2d_core_dir}/ios/Release-iphoneos/libLive2DCubismCore.a")
        endif ()
    else ()
        set(_live2d_core "${_live2d_core_dir}/macos/${_live2d_arch}/libLive2DCubismCore.a")
    endif ()
    set(FRAMEWORK_SOURCE Metal)
elseif (WIN32)
    if (CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_live2d_arch x86_64)
    else ()
        set(_live2d_arch x86)
    endif ()
    set(_live2d_core "${_live2d_core_dir}/windows/${_live2d_arch}/143/Live2DCubismCore_MD.lib")
    set(FRAMEWORK_SOURCE D3D11)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_live2d_core "${_live2d_core_dir}/linux/${CMAKE_SYSTEM_PROCESSOR}/libLive2DCubismCore.a")
    set(FRAMEWORK_SOURCE OpenGL)
    set(_live2d_definitions CSM_TARGET_LINUX_GL)
    find_package(GLEW REQUIRED)
endif ()

if (NOT EXISTS "${_live2d_core}")
    message(FATAL_ERROR "Cubism 5 R5 does not provide the selected Core slice: ${_live2d_core}")
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(_live2d_patch_command patch)
    if (NOT _live2d_patch_command)
        message(FATAL_ERROR "The Linux Cubism renderer patch requires the 'patch' command")
    endif ()

    set(_live2d_renderer_cpp
            "${LIVE2D_CUBISM_NATIVE_ROOT}/Framework/src/Rendering/OpenGL/CubismRenderer_OpenGLES2.cpp")
    set(_live2d_shader_cpp
            "${LIVE2D_CUBISM_NATIVE_ROOT}/Framework/src/Rendering/OpenGL/CubismShader_OpenGLES2.cpp")
    file(READ "${_live2d_renderer_cpp}" _live2d_renderer_source)
    file(READ "${_live2d_shader_cpp}" _live2d_shader_source)
    string(FIND "${_live2d_renderer_source}"
            "LLMSWITCH_CUBISM_LINUX_GLES3_VBO_PATCH" _live2d_renderer_patch_marker)
    string(FIND "${_live2d_shader_source}"
            "LLMSWITCH_CUBISM_LINUX_GLES3_VBO_PATCH" _live2d_shader_patch_marker)
    if (_live2d_renderer_patch_marker EQUAL -1 OR _live2d_shader_patch_marker EQUAL -1)
        execute_process(
                COMMAND "${_live2d_patch_command}" --batch --forward -p1
                    -i "${CMAKE_CURRENT_LIST_DIR}/../patches/cubism-linux-gles3-vbo.patch"
                WORKING_DIRECTORY "${LIVE2D_CUBISM_NATIVE_ROOT}"
                RESULT_VARIABLE _live2d_patch_result
                OUTPUT_VARIABLE _live2d_patch_stdout
                ERROR_VARIABLE _live2d_patch_stderr)
        if (NOT _live2d_patch_result EQUAL 0)
            message(FATAL_ERROR
                    "Could not apply the Linux Cubism GLES VBO patch. The SDK may have changed.\n"
                    "stdout: ${_live2d_patch_stdout}\n"
                    "stderr: ${_live2d_patch_stderr}")
        endif ()
        file(READ "${_live2d_renderer_cpp}" _live2d_renderer_source)
        file(READ "${_live2d_shader_cpp}" _live2d_shader_source)
        string(FIND "${_live2d_renderer_source}"
                "LLMSWITCH_CUBISM_LINUX_GLES3_VBO_PATCH" _live2d_renderer_patch_marker)
        string(FIND "${_live2d_shader_source}"
                "LLMSWITCH_CUBISM_LINUX_GLES3_VBO_PATCH" _live2d_shader_patch_marker)
        if (_live2d_renderer_patch_marker EQUAL -1 OR _live2d_shader_patch_marker EQUAL -1)
            message(FATAL_ERROR "The Linux Cubism GLES VBO patch completed without its expected markers")
        endif ()
    endif ()
endif ()

add_library(live2d_cubism_core STATIC IMPORTED)
set_target_properties(live2d_cubism_core PROPERTIES
        IMPORTED_LOCATION "${_live2d_core}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIVE2D_CUBISM_NATIVE_ROOT}/Core/include")
if (WIN32)
    set_property(TARGET live2d_cubism_core PROPERTY IMPORTED_LOCATION_DEBUG
            "${_live2d_core_dir}/windows/${_live2d_arch}/143/Live2DCubismCore_MDd.lib")
endif ()

add_subdirectory("${LIVE2D_CUBISM_NATIVE_ROOT}/Framework" "${CMAKE_CURRENT_BINARY_DIR}/cubism-framework")
target_link_libraries(Framework PUBLIC live2d_cubism_core)
target_compile_definitions(Framework PUBLIC ${_live2d_definitions})
set_target_properties(Framework PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (APPLE)
    if (IOS)
        set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/../platform/ios/src/metal_surface.mm"
                PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        if (_live2d_apple_sysroot MATCHES "simulator")
            set(PLATFORM_NAME iphonesimulator)
        else ()
            set(PLATFORM_NAME iphoneos)
        endif ()
    else ()
        set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/../platform/macos/src/metal_surface.mm"
                PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        set(PLATFORM_NAME macosx)
    endif ()
    include("${CMAKE_CURRENT_LIST_DIR}/MetalShaders.cmake")
    target_link_libraries(Framework PUBLIC "-framework Metal" "-framework MetalKit" "-framework Foundation")
elseif (ANDROID)
    target_link_libraries(Framework PUBLIC GLESv2 EGL log)
elseif (WIN32)
    target_link_libraries(Framework PUBLIC d3d11 d3dcompiler dxgi)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(OpenGL REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIVE2D_GTK REQUIRED IMPORTED_TARGET "gtk4>=4.6")
    target_link_libraries(huxerui_live2d PRIVATE PkgConfig::LIVE2D_GTK)
    target_link_libraries(Framework PUBLIC GLEW::GLEW OpenGL::GL)
endif ()

target_link_libraries(huxerui_live2d PRIVATE Framework)
target_include_directories(huxerui_live2d PRIVATE "${LIVE2D_CUBISM_NATIVE_ROOT}/Samples/OpenGL/thirdParty/stb")

if (FRAMEWORK_SOURCE STREQUAL "OpenGL" OR FRAMEWORK_SOURCE STREQUAL "D3D11")
    include("${CMAKE_CURRENT_LIST_DIR}/EmbeddedShaders.cmake")
endif ()

if (IOS)
    find_program(_live2d_libtool libtool REQUIRED)
    add_custom_command(TARGET huxerui_live2d POST_BUILD
            COMMAND "${_live2d_libtool}" -static -o "$<TARGET_FILE:huxerui_live2d>.combined"
                "$<TARGET_FILE:huxerui_live2d>" "$<TARGET_FILE:Framework>" "$<TARGET_FILE:live2d_cubism_core>"
            COMMAND "${CMAKE_COMMAND}" -E rename
                "$<TARGET_FILE:huxerui_live2d>.combined" "$<TARGET_FILE:huxerui_live2d>"
            VERBATIM)
endif ()
