#!/bin/bash
# Create a macOS .app bundle for the Model Viewer demo.
#
# Adapted from the runtime repo's installer/macos/create_app_bundle.sh.
# Differences from that template:
#  - No runtime dylib is bundled. The demo defers to the system-installed
#    DisplayXR runtime via /etc/xdg/openxr/1/active_runtime.json (registered
#    by the runtime .pkg's postinstall). The bundled openxr_loader.1.dylib
#    handles standard OpenXR discovery from there.
#  - Launcher does NOT export XR_RUNTIME_JSON. System runtime wins.
#  - The bundled sample.glb scene is copied next to the binary (matches
#    the CMake POST_BUILD step the demo already does).
#  - The DisplayXR app manifest (macos/displayxr/...) and icons are copied
#    into Contents/Resources/displayxr/ so a future macOS shell port can
#    discover them in-bundle.
#
# Usage: ./create_app_bundle.sh <artifact-dir> [output.app]
#   <artifact-dir>: dir containing bin/<binary>, lib/<bundled dylibs>, and
#                   assets/sample.glb + displayxr/{manifest.json,icons}.
set -e

ARTIFACT_DIR="${1:?Usage: $0 <artifact-dir> [output.app]}"
APP_BUNDLE="${2:-3D Model Viewer.app}"
BINARY_NAME="model_viewer_handle_vk_macos"
VERSION="${DISPLAYXR_VERSION:-1.0.0}"

BUNDLE_DISPLAY_NAME="3D Model Viewer"
BUNDLE_ID="com.displayxr.modelviewer"

if [ ! -f "$ARTIFACT_DIR/bin/$BINARY_NAME" ]; then
    echo "Error: $BINARY_NAME binary not found in $ARTIFACT_DIR/bin/" >&2
    exit 1
fi

echo "Creating .app bundle: $APP_BUNDLE"

rm -rf "$APP_BUNDLE"
mkdir -p "$APP_BUNDLE/Contents/MacOS"
mkdir -p "$APP_BUNDLE/Contents/Resources/lib"
mkdir -p "$APP_BUNDLE/Contents/Resources/displayxr"

# --- PkgInfo ---
echo -n "APPL????" > "$APP_BUNDLE/Contents/PkgInfo"

# --- Info.plist ---
cat > "$APP_BUNDLE/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>${BUNDLE_DISPLAY_NAME}</string>
    <key>CFBundleIdentifier</key>
    <string>${BUNDLE_ID}</string>
    <key>CFBundleName</key>
    <string>${BUNDLE_DISPLAY_NAME}</string>
    <key>CFBundleDisplayName</key>
    <string>${BUNDLE_DISPLAY_NAME}</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>${VERSION}</string>
    <key>CFBundleVersion</key>
    <string>${VERSION}</string>
    <key>LSMinimumSystemVersion</key>
    <string>13.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
EOF

# --- Shell launcher (CFBundleExecutable) ---
# Sets DYLD_LIBRARY_PATH so the bundled loader + ICDs are found; does NOT
# set XR_RUNTIME_JSON so the bundled OpenXR loader discovers the
# system-installed DisplayXR runtime via /etc/xdg/openxr/1/active_runtime.json.
cat > "$APP_BUNDLE/Contents/MacOS/$BUNDLE_DISPLAY_NAME" <<'LAUNCHER'
#!/bin/bash
DIR="$(cd "$(dirname "$0")/../Resources" && pwd)"
export DYLD_LIBRARY_PATH="$DIR/lib:${DYLD_LIBRARY_PATH:-}"
export VK_ICD_FILENAMES="$DIR/MoltenVK_icd.json"
export VK_DRIVER_FILES="$DIR/MoltenVK_icd.json"
cd "$DIR"
exec "$DIR/model_viewer_handle_vk_macos" "$@"
LAUNCHER
chmod +x "$APP_BUNDLE/Contents/MacOS/$BUNDLE_DISPLAY_NAME"

# --- Resources: binary, bundled scene, app manifest + icons ---
cp "$ARTIFACT_DIR/bin/$BINARY_NAME" "$APP_BUNDLE/Contents/Resources/"
if [ -f "$ARTIFACT_DIR/assets/sample.glb" ]; then
    cp "$ARTIFACT_DIR/assets/sample.glb" "$APP_BUNDLE/Contents/Resources/"
fi
if [ -d "$ARTIFACT_DIR/displayxr" ]; then
    cp -R "$ARTIFACT_DIR/displayxr/." "$APP_BUNDLE/Contents/Resources/displayxr/"
fi

# --- Resources/lib: bundled support dylibs ---
# The .pkg ships its own copies of these so the .app works on Macs without
# Homebrew. Glob-copy in case the brew install ships symlinks alongside the
# versioned dylib (e.g. libopenxr_loader.dylib → libopenxr_loader.1.dylib).
cp "$ARTIFACT_DIR"/lib/libopenxr_loader*.dylib "$APP_BUNDLE/Contents/Resources/lib/" 2>/dev/null || true
cp "$ARTIFACT_DIR"/lib/libvulkan*.dylib "$APP_BUNDLE/Contents/Resources/lib/" 2>/dev/null || true
cp "$ARTIFACT_DIR"/lib/libMoltenVK*.dylib "$APP_BUNDLE/Contents/Resources/lib/" 2>/dev/null || true

# --- MoltenVK ICD manifest (relative library_path) ---
cat > "$APP_BUNDLE/Contents/Resources/MoltenVK_icd.json" <<'EOF'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "lib/libMoltenVK.dylib",
        "api_version": "1.2.0",
        "is_portability_driver": true
    }
}
EOF

# --- rpath / install_name fix-up ---
# Modern macOS SIGKILLs Mach-O whose code signature was invalidated by
# install_name_tool. Every modification below is immediately followed by an
# ad-hoc codesign. See displayxr-runtime PR #279 for the prior failure mode.

fixup_dylib_id() {
    local dylib="$1"
    local rpath_name="$2"
    if [ -f "$dylib" ]; then
        chmod u+w "$dylib"
        install_name_tool -id "@rpath/$rpath_name" "$dylib"
        codesign --force --sign - "$dylib"
    fi
}

# Set each dylib's install name to @rpath/<basename> so consumers find it
# via the binary's rpath (Contents/Resources/lib).
LIBDIR="$APP_BUNDLE/Contents/Resources/lib"
for d in "$LIBDIR"/*.dylib; do
    [ -e "$d" ] || continue
    bn=$(basename "$d")
    fixup_dylib_id "$d" "$bn"
done

# Rewrite the demo binary's references to point at @rpath/, add an rpath
# rooted at @loader_path/lib so the bundled dylibs resolve, and re-sign.
BIN="$APP_BUNDLE/Contents/Resources/$BINARY_NAME"
chmod u+w "$BIN"

# Pull all current dylib references the binary links against and rewrite
# any non-system ones to @rpath/<basename>. This catches Homebrew paths
# (varies by arch: /opt/homebrew/... on arm64, /usr/local/... on x86_64)
# without hardcoding either.
otool -L "$BIN" | tail -n +2 | awk '{print $1}' | while read -r ref; do
    case "$ref" in
        @rpath/*|@loader_path/*|@executable_path/*|/usr/lib/*|/System/*)
            continue
            ;;
    esac
    case "$ref" in
        *libopenxr_loader*|*libvulkan*|*libMoltenVK*)
            new="@rpath/$(basename "$ref")"
            install_name_tool -change "$ref" "$new" "$BIN"
            ;;
    esac
done

# Drop any absolute build-time rpaths the linker left behind, then add the
# one we actually want. Both are idempotent: -delete_rpath fails silently
# when the rpath isn't present, hence the `|| true`.
otool -l "$BIN" | awk '/LC_RPATH/{f=1} f && /path /{print $2; f=0}' | while read -r rp; do
    case "$rp" in
        @loader_path/lib) continue ;;
        @loader_path*|@executable_path*) install_name_tool -delete_rpath "$rp" "$BIN" 2>/dev/null || true ;;
        /*) install_name_tool -delete_rpath "$rp" "$BIN" 2>/dev/null || true ;;
    esac
done
install_name_tool -add_rpath "@loader_path/lib" "$BIN" 2>/dev/null || true
codesign --force --sign - "$BIN"

echo ".app bundle created: $APP_BUNDLE"
