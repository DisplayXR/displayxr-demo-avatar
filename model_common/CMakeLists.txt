# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Standalone build (displayxr-demo-modelviewer repo). Builds the
# vendor-neutral glTF/PBR model renderer library (`model_renderer`) that the
# platform executables (windows/, macos/) link against. Mirrors the role
# 3dgs_common/ plays in displayxr-demo-gaussiansplat.
#
# The renderer is intended to be ported from the MIT-licensed
# SaschaWillems/Vulkan-glTF-PBR reference (vkglTF::Model loader + PBR/IBL
# shaders). See ../PORTING.md for the exact files to lift.

cmake_minimum_required(VERSION 3.21)
project(model_renderer_lib LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(Vulkan REQUIRED)

# ── glTF 2.0 loading (tinygltf, header-only, MIT) ─────────────────────────
# tinygltf pulls in nlohmann/json + stb internally. Header-only, so we expose
# it as an INTERFACE target and let model_loader.cpp define the implementation
# (TINYGLTF_IMPLEMENTATION) in exactly one translation unit.
include(FetchContent)
FetchContent_Declare(tinygltf
    GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
    GIT_TAG        v2.9.3
)
set(TINYGLTF_HEADER_ONLY  ON  CACHE INTERNAL "" FORCE)
set(TINYGLTF_INSTALL      OFF CACHE INTERNAL "" FORCE)
set(TINYGLTF_BUILD_LOADER_EXAMPLE OFF CACHE INTERNAL "" FORCE)
FetchContent_MakeAvailable(tinygltf)

# ── glm (math, header-only) ───────────────────────────────────────────────
# The PBR reference renderer uses glm for matrices/quaternions. The demo's
# own Kooima view math lives in common/leia_math.h; glm is for the model
# node hierarchy / skinning transforms inside model_common only.
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
)
FetchContent_MakeAvailable(glm)

# ── USD / USDZ loading (tinyusdz, Apache-2.0) ─────────────────────────────
# The one heavy import dependency (vs the vendored single-file OBJ/FBX libs).
# tydra's RenderScene flattens a USD stage into triangulated meshes +
# UsdPreviewSurface materials → mapped 1:1 into ModelData by model_loader_usd.cpp.
# tinyusdz includes <windows.h> without NOMINMAX, whose min/max macros mangle
# its own std::min/std::max calls — define it before adding the subproject.
if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()
FetchContent_Declare(tinyusdz
    GIT_REPOSITORY https://github.com/lighttransport/tinyusdz.git
    GIT_TAG        v0.9.4
)
set(TINYUSDZ_BUILD_TESTS        OFF CACHE INTERNAL "" FORCE)
set(TINYUSDZ_BUILD_EXAMPLES     OFF CACHE INTERNAL "" FORCE)
set(TINYUSDZ_WITH_OPENSUBDIV    OFF CACHE INTERNAL "" FORCE)
set(TINYUSDZ_WITH_AUDIO         OFF CACHE INTERNAL "" FORCE)
set(TINYUSDZ_WITH_EXR           OFF CACHE INTERNAL "" FORCE)
FetchContent_MakeAvailable(tinyusdz)

# ── SPIR-V shader compilation ─────────────────────────────────────────────
find_program(GLSLANGVALIDATOR_COMMAND glslangValidator)
if(NOT GLSLANGVALIDATOR_COMMAND)
    message(FATAL_ERROR "glslangValidator required — install the Vulkan SDK")
endif()

set(SHADER_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
# PBR raster shaders. Port the full set (BRDF-LUT, irradiance, prefiltered-env
# generation) from Vulkan-glTF-PBR when wiring IBL; pbr.{vert,frag} is the
# minimal lit pass to start from.
set(SHADER_SOURCES
    shaders/pbr.vert
    shaders/pbr.frag
    shaders/skybox.frag
    shaders/fullscreen.vert
    shaders/brdf_lut.frag
    shaders/irradiance.frag
    shaders/prefilter.frag
)
# GLSL #include dependencies (force recompile when these change).
set(SHADER_INCLUDES
    ${SHADER_INCLUDE_DIR}/sky.glsl
    ${SHADER_INCLUDE_DIR}/ibl_common.glsl
)

set(SHADER_HEADERS "")
foreach(GLSL_REL ${SHADER_SOURCES})
    get_filename_component(GLSL_NAME ${GLSL_REL} NAME)
    string(MAKE_C_IDENTIFIER ${GLSL_NAME} IDENTIFIER)
    set(VARNAME "${IDENTIFIER}_data")
    set(HEADER "${CMAKE_CURRENT_BINARY_DIR}/${GLSL_NAME}.h")
    set(GLSL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/${GLSL_REL}")
    add_custom_command(
        OUTPUT ${HEADER}
        COMMAND ${GLSLANGVALIDATOR_COMMAND}
            -V --target-env vulkan1.2
            -I${SHADER_INCLUDE_DIR}
            ${GLSL_SRC}
            --vn ${VARNAME}
            -o ${HEADER}
        DEPENDS ${GLSL_SRC} ${SHADER_INCLUDES}
        COMMENT "Compiling SPIR-V: ${GLSL_NAME}"
    )
    list(APPEND SHADER_HEADERS ${HEADER})
endforeach()

add_library(model_renderer STATIC
    model_renderer.h
    model_renderer.cpp
    model_loader.h
    model_loader_backends.h
    model_loader.cpp           # format dispatcher + path helpers
    model_loader_gltf.cpp      # .glb/.gltf backend (tinygltf, defines its impl)
    model_loader_material.h
    model_loader_material.cpp  # shared texture/material shim (OBJ + FBX)
    model_loader_stl.cpp       # .stl backend (hand-rolled, no dep)
    model_loader_obj.cpp       # .obj backend (tinyobjloader, vendored)
    model_loader_fbx.cpp       # .fbx backend (ufbx, vendored)
    third_party/ufbx.c         # ufbx implementation (C TU)
    model_loader_usd.cpp       # .usdz/.usd backend (tinyusdz) — stub
    model_vulkan_utils.h
    model_vulkan_utils.cpp
    ${SHADER_HEADERS}
)

target_include_directories(model_renderer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)
# common/ provides stb_image.h declarations; the stb *implementation* is linked
# from common/d3d11_renderer.cpp into the final executable (see model_loader.cpp).
target_include_directories(model_renderer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${tinyusdz_SOURCE_DIR}/src        # tinyusdz.hh + tydra/render-data.hh
)

target_link_libraries(model_renderer PUBLIC
    Vulkan::Vulkan
    glm::glm
)
target_link_libraries(model_renderer PRIVATE
    tinygltf
    tinyusdz::tinyusdz_static
)
