# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: Apache-2.0
#
# Standalone build (displayxr-demo-* repo). The shared C++ scaffolding
# (logging / input / window / xr_session_common / HUD / D3D11 renderer /
# atlas-capture helper / stb) AND the off-axis Kooima math now come from
# displayxr-common, pinned by tag instead of vendored here (epic #396 W3
# math, W4 scaffolding). Only leia_math.h (app-local reference math) stays
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
FetchContent_Declare(
    displayxr_common
    GIT_REPOSITORY https://github.com/DisplayXR/displayxr-common.git
    GIT_TAG v1.1.1
    GIT_SHALLOW TRUE
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
