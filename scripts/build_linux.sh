#!/usr/bin/env bash
#
# scripts/build_linux.sh — Build the Linux avatar demo binary (build-green, #21).
#
# Mirrors scripts/build_macos.sh per the runtime repo's
# docs/guides/linux-demo-port.md, with the Linux swaps: system Vulkan
# (libvulkan-dev — no MoltenVK, no ICD manifest), a from-source OpenXR loader
# pinned to the SAME release as the runtime (1.1.43), and no installer step
# (Linux packaging is out of scope until on-screen lands).
#
# The avatar demo has no media deps — it's a Vulkan model/PBR renderer — so
# unlike the mediaplayer this script provisions no FFmpeg. The heavy compile
# surface is model_common/ (tinygltf / tinyusdz / ufbx / glm + SPIR-V shaders),
# which is exactly what this build-green pass exercises on Linux.
#
# Usage:
#   ./scripts/build_linux.sh
#
# Env:
#   OPENXR_VERSION   OpenXR-SDK release tag for the loader (default 1.1.43).
#                    Keep this pin equal to the runtime's loader pin; CI runs
#                    this script, so the CI pin follows.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

# --- 0. Build OpenXR loader from source -----------------------------------
# Distro loaders lag; build the pinned Khronos loader and install it under
# /tmp/openxr-install (mirrors build_macos.sh + the runtime repo's
# scripts/build_linux.sh --apps). Cached: skipped if both the .so and the
# CMake package config are already present.
OPENXR_VERSION="${OPENXR_VERSION:-1.1.43}"
OPENXR_DIR="/tmp/openxr-install"
if [ ! -f "$OPENXR_DIR/lib/libopenxr_loader.so" ] || \
   [ ! -f "$OPENXR_DIR/lib/cmake/openxr/OpenXRConfig.cmake" ]; then
    echo "==> Building OpenXR loader $OPENXR_VERSION -> $OPENXR_DIR"
    rm -rf /tmp/openxr-sdk "$OPENXR_DIR"
    git clone --depth 1 --branch "release-$OPENXR_VERSION" \
        https://github.com/KhronosGroup/OpenXR-SDK-Source.git /tmp/openxr-sdk
    cmake -B /tmp/openxr-sdk/build -S /tmp/openxr-sdk -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$OPENXR_DIR" \
        -DBUILD_TESTS=OFF -DBUILD_CONFORMANCE_TESTS=OFF \
        -DBUILD_WITH_SYSTEM_JSONCPP=OFF
    cmake --build /tmp/openxr-sdk/build
    cmake --install /tmp/openxr-sdk/build
else
    echo "==> OpenXR loader cached at $OPENXR_DIR"
fi

# --- 1. cmake build -------------------------------------------------------
# Vulkan via the system libvulkan-dev; OpenXR loader via CMAKE_PREFIX_PATH.
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$OPENXR_DIR"
cmake --build build

BIN="$REPO_ROOT/build/linux/avatar_handle_vk_linux"
[ -x "$BIN" ] || { echo "Error: expected binary not found at $BIN" >&2; exit 1; }

echo ""
echo "Built: $BIN"
echo "Run against a dev runtime: scripts/run_avatar_linux.sh"
