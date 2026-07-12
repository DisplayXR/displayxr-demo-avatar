# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
#
# Standalone build — lives inside displayxr-demo-avatar repo as
# linux/CMakeLists.txt. Assumes sibling dirs: ../common, ../model_common,
# ../openxr_includes. Top-level CMakeLists.txt is expected to have already
# added ../common and ../model_common as subdirectories, exposing the
# `sr_common_base` and `model_renderer` targets.
#
# linux/main.cpp is the desktop-Linux entry point: a minimal Vulkan + OpenXR
# frame loop (HOSTED-NULL windowing — the runtime self-creates its window; the
# app-owned X11 window via XR_DXR_xlib_window_binding is Phase-3 work) driving
# the same cross-platform model_common/ModelRenderer the macOS/Windows peers
# use. Build-green vehicle for the M8 Linux epic (#21, meta runtime #699); see
# docs/guides/linux-demo-port.md in displayxr-runtime.

cmake_minimum_required(VERSION 3.21)

if(NOT UNIX OR APPLE)
    return()
endif()

find_package(Vulkan REQUIRED)
message(STATUS "Found Vulkan: ${Vulkan_LIBRARY}")

# OpenXR loader — CMake package (e.g. from a from-source /tmp/openxr-install via
# CMAKE_PREFIX_PATH) → system library → hard error.
set(OPENXR_FOUND FALSE)
find_package(OpenXR QUIET CONFIG)
if(OpenXR_FOUND)
    set(OPENXR_FOUND TRUE)
    message(STATUS "Found OpenXR via find_package")
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
    message(FATAL_ERROR "OpenXR not found. Build the loader from source and pass "
                        "-DCMAKE_PREFIX_PATH=<install> (see scripts/build_linux.sh).")
endif()

add_executable(avatar_handle_vk_linux
    main.cpp
    # stb_image (read) implementation TU: displayxr::common only ships a
    # macOS-gated impl, so Linux supplies its own (see the file).
    stb_image_impl.cpp
)

target_include_directories(avatar_handle_vk_linux PRIVATE
    ${CMAKE_SOURCE_DIR}/openxr_includes
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/model_common
)

target_link_libraries(avatar_handle_vk_linux PRIVATE
    sr_common_base
    model_renderer
    OpenXR::openxr_loader
    Vulkan::Vulkan
)

# Copy the OpenXR loader .so next to the exe when it's an imported target, so a
# from-source loader runs without extra LD_LIBRARY_PATH setup.
if(TARGET OpenXR::openxr_loader)
    get_target_property(_oxr_loc OpenXR::openxr_loader IMPORTED_LOCATION)
    if(NOT _oxr_loc)
        get_target_property(_oxr_loc OpenXR::openxr_loader IMPORTED_LOCATION_RELEASE)
    endif()
    if(_oxr_loc)
        add_custom_command(TARGET avatar_handle_vk_linux POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_oxr_loc}" "$<TARGET_FILE_DIR:avatar_handle_vk_linux>"
            COMMENT "Copying OpenXR loader next to avatar_handle_vk_linux")
    endif()
endif()

# Copy the bundled avatar (FBX + its flattened texture) next to the exe. The
# FBX references its texture by a stale relative path; model_loader_fbx's
# basename fallback resolves rgb.jpg from the model's own directory, so both
# files must sit side-by-side next to the exe. Mirrors macos/CMakeLists.txt.
set(AVATAR_DIR "${CMAKE_SOURCE_DIR}/assets/tiger")
if(EXISTS "${AVATAR_DIR}/avatar.fbx")
    add_custom_command(TARGET avatar_handle_vk_linux POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${AVATAR_DIR}/avatar.fbx" "$<TARGET_FILE_DIR:avatar_handle_vk_linux>/avatar.fbx"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${AVATAR_DIR}/rgb.jpg"   "$<TARGET_FILE_DIR:avatar_handle_vk_linux>/rgb.jpg"
        COMMENT "Copying bundled avatar.fbx + texture next to exe")
endif()
