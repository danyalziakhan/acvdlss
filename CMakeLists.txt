cmake_minimum_required(VERSION 3.20)
project(acvdlss LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# The NGX static library is built against the static runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# fxc ships with the Windows SDK. Outside a developer prompt, use the newest
# SDK in the default install location.
file(GLOB SDK_BIN LIST_DIRECTORIES true "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64")
list(SORT SDK_BIN)
list(REVERSE SDK_BIN)
find_program(FXC fxc REQUIRED HINTS "$ENV{WindowsSdkVerBinPath}x64" ${SDK_BIN})

set(SHADER_HEADERS)
set(SHADER_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vignette.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/radial.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/lens.hlsl)
foreach(shader prepare rcas downsample screen_effects)
    set(header ${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.h)
    add_custom_command(
        OUTPUT ${header}
        COMMAND ${FXC} /nologo /T cs_5_0 /E main /O3 /Vn ${shader}_cs /Fh ${header} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.hlsl
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.hlsl ${SHADER_INCLUDES}
        COMMENT "Compiling ${shader}.hlsl")
    list(APPEND SHADER_HEADERS ${header})
endforeach()

# acvdlss.ini ships in the zip and is also embedded, so the add-on can write
# it if missing. Same source file, so they can't drift.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/acvdlss.ini)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/acvdlss.ini DEFAULT_INI)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/default_ini.h
     CONTENT "#pragma once\nconst char DEFAULT_INI[] = R\"ini(@DEFAULT_INI@)ini\";\n" @ONLY)

set(NGX_LIB ${CMAKE_CURRENT_SOURCE_DIR}/deps/dlss/lib/Windows_x86_64/x64)

add_library(acvdlss SHARED
    src/acvdlss.cpp
    src/taa.cpp
    src/jitter.cpp
    src/reflex.cpp
    src/config.cpp
    src/ngx_guard.cpp
    src/dlss.cpp
    src/passes.cpp
    src/lod.cpp
    src/log.cpp
    src/descriptors.cpp
    src/acvdlss.rc
    ${SHADER_HEADERS})
target_include_directories(acvdlss PRIVATE
    deps/reshade/include
    deps/imgui
    deps/dlss/include
    ${CMAKE_CURRENT_BINARY_DIR}/shaders
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(acvdlss PRIVATE d3d12 dxgi shell32 ole32 version wintrust
    $<IF:$<CONFIG:Debug>,${NGX_LIB}/nvsdk_ngx_s_dbg.lib,${NGX_LIB}/nvsdk_ngx_s.lib>)
target_compile_definitions(acvdlss PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)

# Leaves out the jitter patch (scans and patches game memory) and the
# signature check. Smart App Control judges unknown unsigned DLLs by their
# contents, and it blocked 0.4.
option(ACVDLSS_PLAIN "Leave out the jitter patch and the signature check" OFF)
if(ACVDLSS_PLAIN)
    target_compile_definitions(acvdlss PRIVATE ACVDLSS_PLAIN)
endif()
target_compile_options(acvdlss PRIVATE /W3 /utf-8)
set_target_properties(acvdlss PROPERTIES SUFFIX ".addon64")
set_target_properties(acvdlss PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
