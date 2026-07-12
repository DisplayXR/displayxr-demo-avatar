#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Run the Linux avatar demo against a local DisplayXR dev runtime.
# Mirrors run_macos_dev.sh, plus the Linux-runtime env the playbook prescribes
# (docs/guides/linux-demo-port.md in the runtime repo): sim-display plug-in
# discovery, the native Vulkan compositor path, and an anaglyph weave so output
# is eyeball-checkable without 3D hardware.
#
# NOTE: on-screen operation is gated on the runtime's Linux Phase 1b/3b
# hardware bring-up. The app is currently HOSTED-NULL (the runtime self-creates
# its window); the faithful app-owned X11 window via XR_DXR_xlib_window_binding
# is Phase-3 work (the header is already vendored in openxr_includes/).
#
# Usage: scripts/run_avatar_linux.sh [model.glb|.gltf|.fbx] [extra args...]
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${REPO_DIR}/build/linux/avatar_handle_vk_linux"
RUNTIME_BUILD="${REPO_DIR}/../displayxr-runtime/build"

# Default to the sibling runtime checkout's dev manifest; override by exporting
# XR_RUNTIME_JSON before invoking.
: "${XR_RUNTIME_JSON:=${RUNTIME_BUILD}/openxr_displayxr-dev.json}"
export XR_RUNTIME_JSON

# Sim-display plug-in discovery (POSIX search path; the runtime's
# scripts/build_linux.sh stages DisplayXR-SimDisplay.so + its manifest here).
: "${XRT_PLUGIN_SEARCH_PATH:=${RUNTIME_BUILD}/_plugins}"
export XRT_PLUGIN_SEARCH_PATH

# Native Vulkan compositor (Phase 1 vk_native + VK_KHR_xcb_surface path).
export OXR_ENABLE_VK_NATIVE_COMPOSITOR="${OXR_ENABLE_VK_NATIVE_COMPOSITOR:-1}"

# Sim-display weave for eyeball checks without 3D hardware.
export SIM_DISPLAY_OUTPUT="${SIM_DISPLAY_OUTPUT:-anaglyph}"

# Script-built OpenXR loader (scripts/build_linux.sh installs it here). Unlike
# Windows/macOS, the exe-adjacent copy isn't on the Linux search path.
export LD_LIBRARY_PATH="/tmp/openxr-install/lib:${LD_LIBRARY_PATH:-}"

if [[ ! -f "${XR_RUNTIME_JSON}" ]]; then
    echo "warning: XR_RUNTIME_JSON not found: ${XR_RUNTIME_JSON}" >&2
    echo "         build the runtime (scripts/build_linux.sh there) or set XR_RUNTIME_JSON." >&2
fi
if [[ ! -x "${BIN}" ]]; then
    echo "error: ${BIN} not built. Run: ./scripts/build_linux.sh" >&2
    exit 1
fi

echo "XR_RUNTIME_JSON=${XR_RUNTIME_JSON}"
echo "XRT_PLUGIN_SEARCH_PATH=${XRT_PLUGIN_SEARCH_PATH}"
exec "${BIN}" "$@"
