#!/bin/bash
# Create a macOS .app bundle for the Avatar demo.
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
APP_BUNDLE="${2:-3D Avatar.app}"
BINARY_NAME="avatar_handle_vk_macos"
VERSION="${DISPLAYXR_VERSION:-1.0.0}"

BUNDLE_DISPLAY_NAME="3D Avatar"
BUNDLE_ID="com.displayxr.avatar"

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
    <key>CFBundleIconFile</key>
    <string>avatar</string>
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
exec "$DIR/avatar_handle_vk_macos" "$@"
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

# --- App icon (.icns) ---
# Generate Contents/Resources/avatar.icns from the 2D workspace logo
# (displayxr/avatar_icon.png) so the .app shows the avatar logo in Finder /
# the Dock. iconutil + sips ship with macOS, so this needs no extra deps and
# keeps the logo single-sourced (no committed .icns binary). CFBundleIconFile
# in Info.plist points at "avatar".
ICON_SRC="$ARTIFACT_DIR/displayxr/avatar_icon.png"
if [ -f "$ICON_SRC" ]; then
    ICONSET="$(mktemp -d)/avatar.iconset"
    mkdir -p "$ICONSET"
    for spec in "16:16x16" "32:16x16@2x" "32:32x32" "64:32x32@2x" \
                "128:128x128" "256:128x128@2x" "256:256x256" "512:256x256@2x" \
                "512:512x512" "1024:512x512@2x"; do
        px="${spec%%:*}"; name="${spec#*:}"
        sips -z "$px" "$px" "$ICON_SRC" --out "$ICONSET/icon_${name}.png" >/dev/null
    done
    iconutil -c icns "$ICONSET" -o "$APP_BUNDLE/Contents/Resources/avatar.icns"
    rm -rf "$(dirname "$ICONSET")"
else
    echo "Warning: $ICON_SRC not found — .app will have no custom icon" >&2
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
