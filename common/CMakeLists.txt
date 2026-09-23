# Copyright 2025, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
#
# Standalone build (displayxr-demo-* repo). The shared C++ scaffolding
# (logging / input / window / xr_session_common / HUD / D3D11 renderer /
# atlas-capture helper / stb) AND the off-axis Kooima math now come from
# displayxr-common, pinned by tag instead of vendored here (epic #396 W3
# math, W4 scaffolding). Only dxr_math.h (app-local reference math) stays
# in this directory.

cmake_minimum_required(VERSION 3.21)
project(sr_common LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# displayxr-common reuses whatever OpenXR target the consumer provides, else
# populates the headers itself; the DisplayXR extension headers come from this
# repo's vendored openxr_includes (keep it refreshed from displayxr-extensions).
include(FetchContent)
set(DISPLAYXR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
get_filename_component(_dxr_ext_includes
    "${CMAKE_CURRENT_SOURCE_DIR}/../openxr_includes" ABSOLUTE)
set(DISPLAYXR_EXTENSIONS_INCLUDE_DIR "${_dxr_ext_includes}" CACHE PATH
    "DisplayXR OpenXR extension headers for displayxr::common")
# (Also pinned in android/src/main/cpp/CMakeLists.txt. That one deliberately
# stays at v2.15.0 for now: the desktop pin moves for displayxr::linux_window,
# which Android does not use, and the Android leg is left untouched.)
FetchContent_Declare(
    displayxr_common
    GIT_REPOSITORY https://github.com/DisplayXR/displayxr-common.git
    # v2.20.0 ships displayxr::linux_window — the one Linux window (X11 or
    # native Wayland in one binary, chosen by capability; transparency,
    # click-through, the phase-snapped drag), shared with the runtime's test
    # apps and the other demos.
    # TEMPORARY: the displayxr-common PR commit (displayxr-common#58); switch to
    # `GIT_TAG v2.20.0` + `GIT_SHALLOW TRUE` once that tag is cut.
    GIT_TAG 327a97b2462d0ccd7602af30831ec27e500ede0a
    GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(displayxr_common)

# Thin compatibility shims: the apps keep linking sr_common (Windows D3D11/HUD
# stack) and sr_common_base (API-agnostic) — both now map onto
# displayxr::common, which carries the platform gating, the d3d11/d2d1/dwrite
# link closure, the dGPU-hint /INCLUDE option, and the single STB
# implementation TUs itself.
add_library(sr_common_base INTERFACE)
target_link_libraries(sr_common_base INTERFACE displayxr::common)
target_include_directories(sr_common_base INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../openxr_includes
)

add_library(sr_common INTERFACE)
target_link_libraries(sr_common INTERFACE sr_common_base)
