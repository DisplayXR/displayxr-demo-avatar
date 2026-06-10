# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Top-level CMakeLists.txt for the displayxr-demo-avatar standalone repo.
# Orchestrates the shared `common` and `model_common` libraries, then
# dispatches to macos/ or windows/ for the platform-specific executable.
#
# Derived from displayxr-demo-modelviewer: same native-Vulkan _handle app and
# PBR/skinning renderer, retargeted to a transparent click-through avatar that
# floats over the desktop with a 2D speech bubble (XR_EXT_local_3d_zone).

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
else()
    message(FATAL_ERROR "displayxr-demo-modelviewer supports macOS and Windows only.")
endif()
