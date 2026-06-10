#!/usr/bin/env bash
#
# scripts/run_macos_dev.sh — launch the locally-built model viewer against an
# already-installed DisplayXR macOS runtime (from the DisplayXRBundle .pkg).
#
# Why this script exists
# ----------------------
# The dev binary links Homebrew's Vulkan loader (an absolute install name),
# while the installed runtime loads ITS bundled libvulkan via @rpath. With two
# different libvulkan images in one process, the VkInstance the app creates is
# foreign to the runtime's loader and xrGetVulkanGraphicsDeviceKHR fails with
# VK_ERROR_INITIALIZATION_FAILED (the compositor then falls back to null).
#
# The fix is to make the app AND the runtime resolve a single shared loader.
# We point the runtime's @rpath libvulkan at Homebrew's loader (the one the app
# hardcodes) via DYLD_LIBRARY_PATH, and select the runtime's bundled MoltenVK
# (a portability driver) via VK_ICD_FILENAMES. DYLD_* survives because this
# script execs the binary DIRECTLY — exactly like the installed .app launcher.
# (Launching through a SIP-protected intermediary such as `nohup` would purge
# DYLD_* and reintroduce the mismatch.)
#
# The distributed .app (built by scripts/build_macos.sh --installer) bundles
# its own self-consistent Vulkan stack and needs none of this — this script is
# only for iterating on a dev build against an installed runtime.
set -euo pipefail

# --- Locate the installed runtime -----------------------------------------
RT=""
for d in "/Library/Application Support/DisplayXR" "$HOME/Library/Application Support/DisplayXR"; do
    if [ -f "$d/openxr_displayxr.json" ]; then RT="$d"; break; fi
done
if [ -z "$RT" ]; then
    echo "Error: DisplayXR runtime not found." >&2
    echo "       Install the macOS bundle (DisplayXRBundle-*.pkg) from" >&2
    echo "       https://github.com/DisplayXR/displayxr-installer/releases" >&2
    exit 1
fi
echo "==> Runtime: $RT"

export XR_RUNTIME_JSON="$RT/openxr_displayxr.json"
export XRT_PLUGIN_SEARCH_PATH="$RT/DisplayProcessors"
export VK_ICD_FILENAMES="$RT/share/vulkan/icd.d/MoltenVK_icd.json"
export VK_DRIVER_FILES="$VK_ICD_FILENAMES"

# Converge app + runtime on one libvulkan (see header).
VK_PREFIX="$(brew --prefix vulkan-loader 2>/dev/null || true)"
if [ -z "$VK_PREFIX" ] || [ ! -d "$VK_PREFIX/lib" ]; then
    echo "Error: Homebrew vulkan-loader not found — \`brew install vulkan-loader\`." >&2
    exit 1
fi
export DYLD_LIBRARY_PATH="$VK_PREFIX/lib:${DYLD_LIBRARY_PATH:-}"

# --- Launch ----------------------------------------------------------------
BIN="$(cd "$(dirname "$0")/.." && pwd)/build/macos/model_viewer_handle_vk_macos"
if [ ! -x "$BIN" ]; then
    echo "Error: $BIN not found — build first: ./scripts/build_macos.sh" >&2
    exit 1
fi
echo "==> Launching $BIN"
exec "$BIN" "$@"
