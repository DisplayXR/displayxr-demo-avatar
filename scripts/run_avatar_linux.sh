#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Run the Linux avatar demo against a local DisplayXR dev runtime.
# Mirrors run_macos_dev.sh, plus the Linux-runtime env the playbook prescribes
# (docs/guides/linux-demo-port.md in the runtime repo): sim-display plug-in
# discovery, the native Vulkan compositor path, and an anaglyph weave so output
# is eyeball-checkable without 3D hardware.
#
# The app creates its own transparent window, X11 or native Wayland
# (--platform=x11|wayland|auto; default auto = native Wayland when the
# compositor is ready, else X11), and hands it to the runtime; it only falls
# back to hosted-NULL when no window system answers.
#
# SIM_DISPLAY_OUTPUT below defaults to the sim-display anaglyph weave, which
# EXERCISES THE PIPELINE AND VALIDATES NOTHING ABOUT WEAVING. sim_display
# composites the views in a way that degrades gracefully under resampling,
# cropping or a wrong origin, so a plausible-looking anaglyph run proves only
# plug-in discovery, session/swapchain creation and that the compositor handed
# the display processor an atlas. Geometric correctness — the exact woven
# texture size, the 1:1 physical-pixel mapping and the interlacing phase a real
# lenticular panel needs — can only be established on vendor hardware. For a
# hardware run, point XRT_PLUGIN_SEARCH_PATH at the Leia plug-in and leave
# SIM_DISPLAY_OUTPUT unset.
#
# App environment (see linux/main.cpp's header for the full list):
#   AVATAR_TRANSPARENT=0     start opaque; Ctrl+T toggles at runtime
#   AVATAR_WINDOW=WxH+X+Y    override the window rect (absolute desktop px);
#                            W×H alone re-centres on the 3D panel
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

# Sim-display weave for eyeball checks without 3D hardware. NOT a weave-geometry
# check — see the header. Set SIM_DISPLAY_OUTPUT= (empty) to leave it alone.
export SIM_DISPLAY_OUTPUT="${SIM_DISPLAY_OUTPUT-anaglyph}"

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
