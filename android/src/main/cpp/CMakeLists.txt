# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Native build for the Android leg of displayxr-demo-avatar (a transparent 3D
# avatar floating over the live Android screen). Reuses the modelviewer/cube
# OpenXR-Android harness (loader → instance → Vulkan device → atlas swapchains →
# session) and binds to the DisplayXR runtime out-of-process (ADR-025). The
# renderer is the SHARED model_common/ sources, referenced by relative path.
#
# Avatar identity = the bundled tiger (assets/tiger/avatar.fbx via ufbx). Unlike
# modelviewer's Android leg (glTF-only), this leg ALSO compiles the FBX backend
# (model_loader_fbx.cpp + vendored ufbx.c). OBJ/STL/USD stay desktop-only — gated
# out in model_loader.cpp under __ANDROID__ to avoid tinyobjloader/tinyusdz.

cmake_minimum_required(VERSION 3.22.1)
project(avatar_vk_android C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)          # model_renderer / glm require C++20
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Repo root is four levels up from android/src/main/cpp.
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(MODEL_COMMON "${REPO_ROOT}/model_common")

# ── glTF 2.0 (tinygltf, header-only; bundles nlohmann/json + stb) ─────────
include(FetchContent)
FetchContent_Declare(tinygltf
    GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
    GIT_TAG        v2.9.3)
set(TINYGLTF_HEADER_ONLY ON CACHE INTERNAL "" FORCE)
set(TINYGLTF_INSTALL OFF CACHE INTERNAL "" FORCE)
set(TINYGLTF_BUILD_LOADER_EXAMPLE OFF CACHE INTERNAL "" FORCE)
FetchContent_MakeAvailable(tinygltf)

# ── glm (math, header-only) ───────────────────────────────────────────────
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1)
FetchContent_MakeAvailable(glm)

# ── stb (stb_truetype.h for the button-bar glyph bake; header-only) ───────
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master)
FetchContent_MakeAvailable(stb)

# ── PBR/IBL shaders → SPIR-V header arrays ────────────────────────────────
find_program(GLSLANGVALIDATOR_COMMAND glslangValidator)
if(NOT GLSLANGVALIDATOR_COMMAND)
    message(FATAL_ERROR "glslangValidator required — install the Vulkan SDK on the build host")
endif()
set(SHADER_DIR "${MODEL_COMMON}/shaders")
set(MV_SHADERS pbr.vert pbr.frag skybox.frag fullscreen.vert
    brdf_lut.frag irradiance.frag prefilter.frag edge_fade.frag)
set(SHADER_HEADERS "")
foreach(S ${MV_SHADERS})
    string(REPLACE "." "_" VN "${S}")          # pbr.vert -> pbr_vert
    set(HDR "${CMAKE_CURRENT_BINARY_DIR}/${S}.h")
    add_custom_command(
        OUTPUT ${HDR}
        COMMAND ${GLSLANGVALIDATOR_COMMAND}
            -V --target-env vulkan1.2 -I${SHADER_DIR}
            ${SHADER_DIR}/${S} --vn ${VN}_data -o ${HDR}
        DEPENDS ${SHADER_DIR}/${S}
                ${SHADER_DIR}/ibl_common.glsl ${SHADER_DIR}/sky.glsl
        COMMENT "Compiling SPIR-V: ${S}")
    list(APPEND SHADER_HEADERS ${HDR})
endforeach()

# ── NativeActivity glue (ships with the NDK) ─────────────────────────────
add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

find_package(OpenXR REQUIRED CONFIG)

add_library(avatar_vk_android SHARED
    main.cpp
    hud_bar.cpp
    stb_impl.cpp
    # ── SHARED renderer (model_common/), glTF + FBX subset ──
    ${MODEL_COMMON}/model_renderer.cpp
    ${MODEL_COMMON}/model_loader.cpp
    ${MODEL_COMMON}/model_loader_gltf.cpp
    ${MODEL_COMMON}/model_loader_fbx.cpp       # .fbx backend (ufbx) — the bundled avatar
    ${MODEL_COMMON}/third_party/ufbx.c         # ufbx implementation (C TU)
    ${MODEL_COMMON}/model_loader_material.cpp
    ${MODEL_COMMON}/model_vulkan_utils.cpp
    ${SHADER_HEADERS})

target_include_directories(avatar_vk_android PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}                 # generated *.vert.h / *.frag.h
    ${MODEL_COMMON}                             # model_renderer.h, model_loader.h, ...
    ${tinygltf_SOURCE_DIR}                      # tiny_gltf.h + json.hpp + stb_image.h
    ${glm_SOURCE_DIR}                           # glm/ (header-only)
    ${stb_SOURCE_DIR}                           # stb_truetype.h (button bar)
    ${REPO_ROOT}/openxr_includes)               # DisplayXR XR_EXT_* headers

set_target_properties(avatar_vk_android PROPERTIES
    LINK_FLAGS "-u ANativeActivity_onCreate")

target_link_libraries(avatar_vk_android
    native_app_glue
    OpenXR::openxr_loader
    vulkan
    android
    log)
