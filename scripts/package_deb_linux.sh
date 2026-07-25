#!/usr/bin/env bash
# Package the Linux demo as a Debian package (.deb) — runtime #781 Phase 3
# (demo installers). Companion to build_linux.sh: builds the demo (if needed)
# then wraps the binary + its OpenXR loader + assets into an installable .deb.
#
#   ./scripts/package_deb_linux.sh            # build (if needed) + package
#   ./scripts/package_deb_linux.sh --no-build # package an existing build/
#
# Output: dist/<pkg>_<ver>_<arch>.deb
#
# Payload (installed layout):
#   /usr/lib/displayxr-demos/<app>/<binary>          the demo executable
#   /usr/lib/displayxr-demos/<app>/libopenxr_loader.so*  bundled OpenXR loader
#   /usr/lib/displayxr-demos/<app>/assets/...         bundled sample assets
#   /usr/bin/<pkg>                                    launcher wrapper (on PATH)
#   /usr/share/applications/<pkg>.desktop            menu entry
#
# The demo is an OpenXR app → Depends: displayxr-runtime. With the runtime .deb
# installed, the loader resolves the runtime via /etc/xdg/openxr/1/active_runtime.json
# automatically — no env vars. The wrapper sets LD_LIBRARY_PATH for the bundled
# loader and OXR_ENABLE_VK_NATIVE_COMPOSITOR=1 (Linux vk_native path).
#
# --- Per-demo config (the ONLY part that differs between demos) -------------
APP="avatar"                                        # short id (dir + component)
PKG="displayxr-avatar"                              # .deb package + wrapper name
DISPLAY_NAME="DisplayXR 3D Avatar"
DESCRIPTION="Skeletal-animated glTF avatar/character viewer for glasses-free 3D displays."
BINARY="avatar_handle_vk_linux"                     # build/linux/<BINARY>
DESKTOP_CATEGORIES="Graphics;Viewer;"
ASSETS_SUBDIR="assets"                              # repo dir to bundle, "" if none

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
DIST_DIR="${DIST_DIR:-$ROOT/dist}"
OPENXR_DIR="${OPENXR_DIR:-/tmp/openxr-install}"

NO_BUILD=0
for a in "$@"; do case "$a" in
  --no-build) NO_BUILD=1 ;;
  *) echo "Unknown option: $a (supported: --no-build)" >&2; exit 2 ;;
esac; done

command -v dpkg-deb >/dev/null 2>&1 || { echo "error: dpkg-deb not found — Debian/Ubuntu host/container only." >&2; exit 1; }

BIN="$BUILD_DIR/linux/$BINARY"
if [ ! -x "$BIN" ]; then
  [ "$NO_BUILD" = 0 ] || { echo "error: $BIN not built and --no-build set." >&2; exit 1; }
  echo "==> Building via scripts/build_linux.sh"
  "$ROOT/scripts/build_linux.sh"
fi
[ -x "$BIN" ] || { echo "error: demo binary $BIN missing after build." >&2; exit 1; }

# --- Version: git describe → Debian-legal upstream version (v* tags only) ---
RAW="$(git -C "$ROOT" describe --tags --always --dirty --match 'v[0-9]*' 2>/dev/null || echo 0.0.0)"
VERSION="$(echo "$RAW" | sed -e 's/^v//' -e 's/-dirty$/+dirty/' -e 's/-\([0-9]\+\)-g/+\1.g/')"
case "$VERSION" in [0-9]*) : ;; *) VERSION="0.0.0+g$VERSION" ;; esac
ARCH="$(dpkg --print-architecture)"

STAGE="$DIST_DIR/${PKG}_${VERSION}_${ARCH}"
APPDIR="$STAGE/usr/lib/displayxr-demos/$APP"
echo "==> Staging $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE/DEBIAN" "$APPDIR" "$STAGE/usr/bin" "$STAGE/usr/share/applications"

install -m 0755 "$BIN" "$APPDIR/$BINARY"

# Bundle the OpenXR loader the demo links (built from source by build_linux.sh),
# so the .deb doesn't depend on a distro loader whose version may differ from
# the demo's pin. The wrapper puts $APPDIR on LD_LIBRARY_PATH.
for f in "$OPENXR_DIR"/lib/libopenxr_loader.so*; do
  [ -e "$f" ] && cp -a "$f" "$APPDIR/"
done
[ -e "$APPDIR/libopenxr_loader.so.1" ] || echo "warn: no bundled OpenXR loader — the demo may need a system libopenxr-loader1." >&2

# Bundle assets (sample models etc.).
if [ -n "$ASSETS_SUBDIR" ] && [ -d "$ROOT/$ASSETS_SUBDIR" ]; then
  cp -aR "$ROOT/$ASSETS_SUBDIR" "$APPDIR/assets"
fi

# Launcher wrapper on PATH.
cat > "$STAGE/usr/bin/$PKG" <<EOF
#!/bin/sh
# DisplayXR demo launcher. The runtime .deb registers the OpenXR ActiveRuntime,
# so no env vars are needed; we only wire the bundled loader + the Linux
# vk_native compositor.
DIR="/usr/lib/displayxr-demos/$APP"
export LD_LIBRARY_PATH="\$DIR:\${LD_LIBRARY_PATH:-}"
export OXR_ENABLE_VK_NATIVE_COMPOSITOR="\${OXR_ENABLE_VK_NATIVE_COMPOSITOR:-1}"
exec "\$DIR/$BINARY" "\$@"
EOF
chmod 0755 "$STAGE/usr/bin/$PKG"

# Desktop menu entry.
cat > "$STAGE/usr/share/applications/$PKG.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=$DISPLAY_NAME
Comment=$DESCRIPTION
Exec=$PKG
Terminal=false
Categories=$DESKTOP_CATEGORIES
EOF

# --- Depends: displayxr-runtime (OpenXR runtime + DP) + the binary's + the
# loader's DT_NEEDED system libs (usr-merge-safe; excludes 32-bit multiarch). --
compute_lib_depends() {
  local sonames so pkg pkgs=""
  sonames="$(objdump -p "$APPDIR/$BINARY" "$APPDIR"/libopenxr_loader.so* 2>/dev/null | awk '/NEEDED/{print $2}' | sort -u)"
  for so in $sonames; do
    # skip the bundled loader itself
    case "$so" in libopenxr_loader.so*) continue ;; esac
    pkg="$(dpkg -S "$so" 2>/dev/null \
           | awk -F': ' -v s="$so" '$2 ~ /^\/(usr\/)?lib(32|64)?\// && $2 !~ /(i386-linux-gnu|\/lib32\/|\/libx32\/)/ {n=split($2,a,"/"); if (a[n]==s){p=$1; sub(/:.*/,"",p); print p; exit}}')"
    [ -n "$pkg" ] && pkgs="$pkgs $pkg"
  done
  echo "libc6 $pkgs" | tr ' ' '\n' | sed '/^$/d' | sort -u | paste -sd, - | sed 's/,/, /g'
}
LIB_DEPENDS="$(compute_lib_depends)"
DEPENDS="displayxr-runtime, $LIB_DEPENDS"
echo "==> Depends: $DEPENDS"
INSTALLED_KB="$(du -sk "$STAGE/usr" | cut -f1)"

cat > "$STAGE/DEBIAN/control" <<EOF
Package: $PKG
Version: $VERSION
Section: graphics
Priority: optional
Architecture: $ARCH
Depends: $DEPENDS
Installed-Size: $INSTALLED_KB
Maintainer: The DisplayXR Project <noreply@displayxr.dev>
Homepage: https://github.com/DisplayXR/displayxr-demo-$APP
Description: $DISPLAY_NAME
 $DESCRIPTION
 .
 A DisplayXR demo OpenXR app for glasses-free 3D displays. Requires the
 DisplayXR runtime (Depends: displayxr-runtime); runs on whichever display
 processor is active (sim-display fallback, or the Leia SR plug-in when
 installed). Launch from the menu or run '$PKG'.
EOF

mkdir -p "$DIST_DIR"
DEB="$DIST_DIR/${PKG}_${VERSION}_${ARCH}.deb"
if command -v fakeroot >/dev/null 2>&1; then
  fakeroot dpkg-deb --build --root-owner-group "$STAGE" "$DEB"
else
  dpkg-deb --build --root-owner-group "$STAGE" "$DEB"
fi

echo ""
echo "==> $DEB"
dpkg-deb --info "$DEB" | sed 's/^/    /'
dpkg-deb --contents "$DEB" | sed 's/^/    /'
