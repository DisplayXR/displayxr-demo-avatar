#!/usr/bin/env bash
# Build the macOS .pkg for the Model Viewer demo.
#
# Usage:
#   DISPLAYXR_VERSION=1.4.0 ./build_installer.sh <artifact-dir> [output.pkg]
#
# <artifact-dir> must contain:
#   bin/model_viewer_handle_vk_macos        the cmake-built binary
#   lib/libopenxr_loader*.dylib                   bundled OpenXR loader
#   lib/libvulkan*.dylib                          bundled Vulkan loader
#   lib/libMoltenVK*.dylib                        bundled MoltenVK ICD
#   assets/sample.glb                          bundled default scene
#   displayxr/...                                 app manifest + icons
#
# Two-stage build (mirroring displayxr-runtime/installer/macos/build_installer.sh):
#   1. pkgbuild --root <staging>  → modelviewer.pkg   (component .pkg)
#   2. productbuild --distribution Distribution.xml ... → DisplayXRModelViewer-<v>.pkg
#
# The .pkg is ad-hoc signed only (Signature=adhoc). Full Developer ID signing
# is a parallel effort tracked in DisplayXR/displayxr-runtime#280.
#
# Runtime dependency: this .pkg expects the DisplayXR runtime .pkg to be
# installed first. The runtime's postinstall registers
# /etc/xdg/openxr/1/active_runtime.json, which the bundled OpenXR loader
# uses to find the system runtime. Bundling the runtime dylib here would
# create version-skew risk.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ARTIFACT_DIR="${1:?Usage: $0 <artifact-dir> [output.pkg]}"
VERSION="${DISPLAYXR_VERSION:-1.0.0}"
OUTPUT_PKG="${2:-$(pwd)/DisplayXRModelViewer-${VERSION}.pkg}"

# Resolve to absolute paths up-front so cd around doesn't break them.
ARTIFACT_DIR="$(cd "$ARTIFACT_DIR" && pwd)"
OUTPUT_PKG_DIR="$(cd "$(dirname "$OUTPUT_PKG")" && pwd)"
OUTPUT_PKG="$OUTPUT_PKG_DIR/$(basename "$OUTPUT_PKG")"
mkdir -p "$OUTPUT_PKG_DIR"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

echo "==> Building DisplayXRModelViewer installer"
echo "    version:     $VERSION"
echo "    artifact:    $ARTIFACT_DIR"
echo "    output:      $OUTPUT_PKG"
echo "    workdir:     $WORK_DIR"

# --- Stage payload --------------------------------------------------------
# pkgbuild --root <root> --install-location / lays $root's children at /.
# We want the .app at /Applications/3D Model Viewer.app, so the
# staging tree is $WORK_DIR/payload/Applications/<name>.app.
PAYLOAD_ROOT="$WORK_DIR/payload"
APPS_DIR="$PAYLOAD_ROOT/Applications"
mkdir -p "$APPS_DIR"

# create_app_bundle.sh builds the .app in $PWD; cd to APPS_DIR so the
# bundle ends up there directly. The script needs DISPLAYXR_VERSION in the
# environment; we already exported it (or it inherits).
( cd "$APPS_DIR" && DISPLAYXR_VERSION="$VERSION" \
    bash "$SCRIPT_DIR/create_app_bundle.sh" "$ARTIFACT_DIR" "3D Model Viewer.app" )

# Strip stray AppleDouble metadata / `.DS_Store` files from the staging
# tree if any crept in via cp. Note: pkgbuild's cpio path still encodes
# resource forks for code-signed Mach-O as `._<name>` entries inside the
# Payload; those re-appear on extraction but are functionally harmless.
find "$PAYLOAD_ROOT" \( -name '._*' -o -name '.DS_Store' \) -delete

# --- pkgbuild: component package ------------------------------------------
COMPONENT_PKG="$WORK_DIR/modelviewer.pkg"
pkgbuild \
    --root "$PAYLOAD_ROOT" \
    --identifier com.displayxr.modelviewer \
    --version "$VERSION" \
    --install-location / \
    "$COMPONENT_PKG"

# --- productbuild: distribution wrapper -----------------------------------
DIST_XML="$SCRIPT_DIR/Distribution.xml"
RESOURCES_DIR="$SCRIPT_DIR/resources"

if [ ! -f "$DIST_XML" ]; then
    echo "Error: $DIST_XML not found" >&2
    exit 1
fi
if [ ! -d "$RESOURCES_DIR" ]; then
    echo "Error: $RESOURCES_DIR not found" >&2
    exit 1
fi

productbuild \
    --distribution "$DIST_XML" \
    --resources "$RESOURCES_DIR" \
    --package-path "$WORK_DIR" \
    "$OUTPUT_PKG"

echo "==> Built: $OUTPUT_PKG"
ls -lh "$OUTPUT_PKG"
