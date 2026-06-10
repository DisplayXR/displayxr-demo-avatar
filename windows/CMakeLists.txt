# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Standalone build — lives inside displayxr-demo-modelviewer repo as
# windows/CMakeLists.txt. Assumes sibling dirs: ../common, ../model_common,
# ../openxr_includes. Top-level CMakeLists.txt is expected to have already
# added ../common and ../model_common as subdirectories, exposing the
# `sr_common_base`, `sr_common`, and `model_renderer` targets.
#
# windows/main.cpp + xr_session.cpp originated from displayxr-demo-gaussiansplat;
# the window/OpenXR/HUD/input glue is reused as-is and the renderer call sites
# are retargeted to model_common/ModelRenderer (glTF PBR). See ../PORTING.md
# for what's done (Windows v1) and what's still open (textures/IBL, macOS).

cmake_minimum_required(VERSION 3.21)

if(NOT WIN32)
    return()
endif()

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
    message(STATUS "Vulkan SDK not found — skipping avatar_handle_vk_win")
    return()
endif()
message(STATUS "Found Vulkan: ${Vulkan_LIBRARY}")

# OpenXR loader — CONFIG pkg first, then a user-specified OpenXR_ROOT fallback.
set(OPENXR_FOUND FALSE)
find_package(OpenXR QUIET CONFIG)
if(OpenXR_FOUND)
    set(OPENXR_FOUND TRUE)
    message(STATUS "Found OpenXR via find_package")
endif()

if(NOT OPENXR_FOUND AND DEFINED OpenXR_ROOT)
    find_library(OPENXR_LOADER_LIB
        NAMES openxr_loader openxr_loader-1_0
        PATHS "${OpenXR_ROOT}/x64/lib" "${OpenXR_ROOT}/lib" "${OpenXR_ROOT}/bin"
        NO_DEFAULT_PATH
    )
    if(OPENXR_LOADER_LIB)
        set(OPENXR_FOUND TRUE)
        add_library(OpenXR::openxr_loader SHARED IMPORTED)
        find_file(OPENXR_LOADER_DLL
            NAMES openxr_loader.dll openxr_loader-1_0.dll
            PATHS "${OpenXR_ROOT}/x64/bin" "${OpenXR_ROOT}/bin"
            NO_DEFAULT_PATH
        )
        if(OPENXR_LOADER_DLL)
            set_target_properties(OpenXR::openxr_loader PROPERTIES
                IMPORTED_LOCATION "${OPENXR_LOADER_DLL}"
                IMPORTED_IMPLIB "${OPENXR_LOADER_LIB}"
            )
        else()
            set_target_properties(OpenXR::openxr_loader PROPERTIES
                IMPORTED_IMPLIB "${OPENXR_LOADER_LIB}"
            )
        endif()
    endif()
endif()

if(NOT OPENXR_FOUND)
    message(FATAL_ERROR "OpenXR not found. Provide -DOpenXR_ROOT=<path> or install via vcpkg.")
endif()

add_executable(avatar_handle_vk_win WIN32
    main.cpp
    xr_session.cpp
    xr_session.h
    resource.rc
)

target_include_directories(avatar_handle_vk_win PRIVATE
    ${CMAKE_SOURCE_DIR}/openxr_includes
)

target_link_libraries(avatar_handle_vk_win PRIVATE
    sr_common_base
    sr_common
    model_renderer
    OpenXR::openxr_loader
    Vulkan::Vulkan
    d3d11
    d2d1
    dwrite
    comdlg32
)

if(MSVC)
    # /utf-8: tell MSVC the source files are UTF-8 so multi-byte glyphs like
    # "…" in HUD button labels aren't mangled at compile time.
    target_compile_options(avatar_handle_vk_win PRIVATE /utf-8)

    file(RELATIVE_PATH _manifest_rel
        "${CMAKE_BINARY_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/avatar_openxr_ext_vk.manifest")
    set_target_properties(avatar_handle_vk_win PROPERTIES
        LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:${_manifest_rel}"
    )
endif()

# Copy OpenXR loader DLL next to exe. Use $<TARGET_FILE:...> rather than the
# IMPORTED_LOCATION property: the Khronos OpenXR CONFIG package only sets the
# *config-specific* IMPORTED_LOCATION_<CONFIG> (e.g. IMPORTED_LOCATION_RELWITHDEBINFO),
# leaving the generic IMPORTED_LOCATION unset. get_target_property(... IMPORTED_LOCATION)
# would return NOTFOUND, silently skipping the copy and shipping an exe with no
# loader next to it (the installer's `File openxr_loader.dll` then fails). The
# generator expression resolves the correct per-config DLL path.
if(TARGET OpenXR::openxr_loader)
    add_custom_command(TARGET avatar_handle_vk_win POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:OpenXR::openxr_loader>"
            "$<TARGET_FILE_DIR:avatar_handle_vk_win>"
        COMMENT "Copying OpenXR loader DLL"
    )
endif()

# Vendor weaver DLLs (e.g. SimulatedRealityVulkanBeta.dll) are intentionally
# NOT bundled — they ship with the vendor plug-in installer and must not
# appear in a vendor-neutral DisplayXR demo.

# Copy the bundled default avatar (FBX + its flattened texture) next to the exe.
# The FBX references its texture by a stale relative path; model_loader_fbx's
# basename fallback resolves rgb.jpg from the model's own directory, so both
# files must sit side-by-side next to the exe.
set(AVATAR_DIR "${CMAKE_SOURCE_DIR}/assets/tiger")
if(EXISTS "${AVATAR_DIR}/avatar.fbx")
    add_custom_command(TARGET avatar_handle_vk_win POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${AVATAR_DIR}/avatar.fbx" "$<TARGET_FILE_DIR:avatar_handle_vk_win>/avatar.fbx"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${AVATAR_DIR}/rgb.jpg"   "$<TARGET_FILE_DIR:avatar_handle_vk_win>/rgb.jpg"
        COMMENT "Copying bundled avatar.fbx + texture next to exe"
    )
endif()

# Copy the DisplayXR shell sidecar (.displayxr.json + icons) next to the exe
# so the installer / shell scanner picks it up.
displayxr_install_manifest(avatar_handle_vk_win
    "${CMAKE_CURRENT_SOURCE_DIR}/displayxr")
