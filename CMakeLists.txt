# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
#
# Top-level CMakeLists.txt for the displayxr-demo-avatar standalone repo.
# Orchestrates the shared `common` and `model_common` libraries, then
# dispatches to macos/ or windows/ for the platform-specific executable.
#
# Derived from displayxr-demo-modelviewer: same native-Vulkan _handle app and
# PBR/skinning renderer, retargeted to a transparent click-through avatar that
# floats over the desktop with a 2D speech bubble (XR_DXR_local_3d_zone).

cmake_minimum_required(VERSION 3.21)
project(displayxr-demo-avatar LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared libraries (built once, linked by the platform executable).
add_subdirectory(common)
add_subdirectory(model_common)

# Platform-specific executable.
if(APPLE)
    add_subdirectory(macos)
elseif(WIN32)
    add_subdirectory(windows)
elseif(UNIX)
    # Desktop Linux — build-green vehicle for the M8 Linux epic (#21). Hosted-NULL
    # windowing; the app-owned X11 window (XR_DXR_xlib_window_binding) is Phase 3.
    add_subdirectory(linux)
else()
    message(FATAL_ERROR "displayxr-demo-avatar supports macOS, Windows and Linux only.")
endif()
