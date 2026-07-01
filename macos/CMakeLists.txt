# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: Apache-2.0
#
# Standalone build — lives inside displayxr-demo-avatar repo as
# macos/CMakeLists.txt. Assumes sibling dirs: ../common, ../model_common,
# ../openxr_includes. Top-level CMakeLists.txt is expected to have already
# added ../common and ../model_common as subdirectories, exposing the
# `sr_common_base` and `model_renderer` targets.
#
# macos/main.mm is the macOS entry point: Cocoa window + MoltenVK + OpenXR
# session + HUD/input, with the renderer call sites wired to ModelRenderer
# (loadModel / hasModel / getRobustSceneBounds, .glb/.gltf NSOpenPanel +
# drag-drop). Transparent-background mode IS supported on macOS: the Metal
# compositor is alpha-native (no chroma key) — see XR_EXT_cocoa_window_binding
# transparentBackgroundEnabled in main.mm + the gauss demo recipe.

cmake_minimum_required(VERSION 3.21)

if(NOT APPLE)
    return()
endif()

find_package(Vulkan REQUIRED)
message(STATUS "Found Vulkan: ${Vulkan_LIBRARY}")

# OpenXR loader — system install (brew), find_package, or user-provided root.
set(OPENXR_FOUND FALSE)
find_package(OpenXR QUIET CONFIG)
if(OpenXR_FOUND)
    set(OPENXR_FOUND TRUE)
endif()

if(NOT OPENXR_FOUND)
    find_library(OPENXR_LOADER_LIB NAMES openxr_loader)
    if(OPENXR_LOADER_LIB)
        set(OPENXR_FOUND TRUE)
        add_library(OpenXR::openxr_loader SHARED IMPORTED)
        set_target_properties(OpenXR::openxr_loader PROPERTIES
            IMPORTED_LOCATION "${OPENXR_LOADER_LIB}"
        )
        message(STATUS "Found OpenXR loader: ${OPENXR_LOADER_LIB}")
    endif()
endif()

if(NOT OPENXR_FOUND)
    message(FATAL_ERROR "OpenXR not found. Install via: brew install openxr-loader")
endif()

add_executable(avatar_handle_vk_macos
    main.mm
    # stb_image (read) implementation TU comes from displayxr::common
    # (stb_image_impl_macos.cpp) — model_loader.cpp takes declarations only
    # (#396 W4).
)

target_include_directories(avatar_handle_vk_macos PRIVATE
    ${CMAKE_SOURCE_DIR}/openxr_includes
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/model_common
)

target_link_libraries(avatar_handle_vk_macos PRIVATE
    sr_common_base
    model_renderer
    OpenXR::openxr_loader
    Vulkan::Vulkan
    "-framework Cocoa"
    "-framework QuartzCore"
    "-framework CoreGraphics"
    "-framework CoreText"
    "-framework CoreFoundation"
    "-framework UniformTypeIdentifiers"
)

# Copy the bundled avatar next to the exe (auto-loaded at startup). The FBX's
# embedded texture path is stale (authored on another machine), so the loader's
# basename fallback resolves rgb.jpg from the model's own directory — copy both.
# Mirrors windows/CMakeLists.txt.
set(AVATAR_DIR "${CMAKE_SOURCE_DIR}/assets/tiger")
if(EXISTS "${AVATAR_DIR}/avatar.fbx")
    add_custom_command(TARGET avatar_handle_vk_macos POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${AVATAR_DIR}/avatar.fbx" "$<TARGET_FILE_DIR:avatar_handle_vk_macos>/avatar.fbx"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${AVATAR_DIR}/rgb.jpg"   "$<TARGET_FILE_DIR:avatar_handle_vk_macos>/rgb.jpg"
        COMMENT "Copying bundled avatar.fbx + texture next to exe"
    )
endif()
