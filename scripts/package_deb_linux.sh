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
ASSETS_SUBDIR=""                                    # repo dir to bundle under assets/, "" if none
# Default asset(s) the app auto-loads at startup, staged FLAT next to the binary.
# The Linux app resolves its bundled model via /proc/self/exe → <exe_dir>/avatar.fbx
# (NOT an assets/ subdir), and the .fbx references rgb.jpg from the same dir — the
# same place the Windows NSI drops both in $INSTDIR. Without this the app finds no
# model and launches empty. Deliberately defaults-only (no assets/ dir bundling).
# "repo-relative-src:installed-basename" pairs.
DEFAULT_ASSETS=("assets/tiger/avatar.fbx:avatar.fbx" "assets/tiger/rgb.jpg:rgb.jpg")

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

# Bundle assets (extra sample models etc.) under assets/.
if [ -n "$ASSETS_SUBDIR" ] && [ -d "$ROOT/$ASSETS_SUBDIR" ]; then
  cp -aR "$ROOT/$ASSETS_SUBDIR" "$APPDIR/assets"
fi

# Stage the default asset(s) FLAT next to the binary. The app resolves its
# bundled model at <exe_dir>/<name> (via /proc/self/exe), so these must sit
# beside the binary, NOT under assets/. Mirrors the Windows installer layout.
for pair in "${DEFAULT_ASSETS[@]:-}"; do
  [ -n "$pair" ] || continue
  src="$ROOT/${pair%%:*}"; dst="${pair##*:}"
  if [ -f "$src" ]; then
    install -m 0644 "$src" "$APPDIR/$dst"
    echo "==> default asset staged: $dst ($(du -h "$src" | cut -f1))"
  else
    echo "warn: default asset '${pair%%:*}' not found — the demo launches with NO bundled model." >&2
  fi
done

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

# --- Depends: ONE .deb for Ubuntu 22.04, 24.04 and 26.04 --------------------
# Two independent things decide whether a .deb installs AND runs on a release,
# and neither is trusted to the build host here (runtime #1656, its PR #1659):
#
#   1. The glibc / libstdc++ floor is the BUILD host's. Release artifacts are
#      built on the OLDEST supported release (the CI Deb job runs in an
#      ubuntu:22.04 container), and dpkg-shlibdeps turns the symbol versions
#      the binaries actually reference into VERSIONED Depends. A package built
#      on a newer host then says `libc6 (>= 2.38)` and apt REFUSES it on 22.04,
#      instead of installing a demo that dies at exec with
#      "version `GLIBC_2.38' not found" (what the unversioned `libc6` from the
#      old dpkg -S mapping did).
#      DXR_DEB_MAX_GLIBC (CI sets 2.35 = Ubuntu 22.04) makes a floor above the
#      oldest supported release a hard error at package time.
#
#   2. The package NAME of a system library is not stable across releases (the
#      t64 transition renamed many: libcurl4 -> libcurl4t64, and the FFmpeg
#      sonames move every release). So every DT_NEEDED soname must be on
#      STABLE_SONAMES — libraries whose package name is identical on 22.04,
#      24.04 and 26.04. A newly linked library fails the build here rather than
#      silently narrowing the releases the package can install on. Adding an
#      entry is a claim about all three releases, and CI's DebInstall matrix
#      (scripts/verify_deb_install_linux.sh) is what proves it.
STABLE_SONAMES=(
    libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 ld-linux-x86-64.so.2
    libstdc++.so.6 libgcc_s.so.1
    libvulkan.so.1                  # libvulkan1
    libX11.so.6 libX11-xcb.so.1     # libx11-6, libx11-xcb1
    libXext.so.6                    # libxext6
    libXrandr.so.2                  # libxrandr2
    libxcb.so.1                     # libxcb1
    libwayland-client.so.0          # libwayland-client0 (native-Wayland window, displayxr::linux_window)
    libxkbcommon.so.0               # libxkbcommon0 (Wayland keysyms)
    libdbus-1.so.3                  # libdbus-1-3 (Wayland drag lattice)
    libz.so.1                       # zlib1g
)

command -v dpkg-shlibdeps >/dev/null 2>&1 || { echo "error: dpkg-shlibdeps not found — install dpkg-dev." >&2; exit 1; }
command -v objdump >/dev/null 2>&1 || { echo "error: objdump not found — install binutils." >&2; exit 1; }

# Every ELF the .deb ships: the demo binary plus the bundled OpenXR loader.
mapfile -t ELF_FILES < <(find "$APPDIR" -type f \( -name "$BINARY" -o -name 'lib*.so*' \) ! -type l | sort)
[ "${#ELF_FILES[@]}" -ge 1 ] || { echo "error: no ELF payload found under $APPDIR." >&2; exit 1; }

bad=""
for so in $(objdump -p "${ELF_FILES[@]}" | awk '/NEEDED/{print $2}' | sort -u); do
    # The OpenXR loader is bundled inside the package, not a system dependency.
    case "$so" in libopenxr_loader.so*) continue ;; esac
    ok=0
    for s in "${STABLE_SONAMES[@]}"; do [ "$so" = "$s" ] && ok=1 && break; done
    [ "$ok" = 1 ] || bad="$bad $so"
done
if [ -n "$bad" ]; then
    echo "error: DT_NEEDED on system libraries not known to share a package name across" >&2
    echo "       Ubuntu 22.04/24.04/26.04:$bad" >&2
    echo "       Drop the dependency, link it statically, bundle it, or add it to" >&2
    echo "       STABLE_SONAMES once CI's DebInstall matrix proves the package exists" >&2
    echo "       under that name on all three releases." >&2
    exit 1
fi

# dpkg-shlibdeps wants a debian/control to read; give it a throwaway one. It
# resolves each soname through the linker search path and the owning package's
# shlibs/symbols files, so the result carries real version floors.
# -l"$APPDIR": the bundled OpenXR loader lives there (the launcher puts it on
# LD_LIBRARY_PATH); --ignore-missing-info keeps that package-less private lib
# from aborting the run — an unknown SYSTEM soname is caught by STABLE_SONAMES
# above, not here.
SHLIBS_TMP="$(mktemp -d)"
mkdir -p "$SHLIBS_TMP/debian"
printf 'Source: %s\n\nPackage: %s\nArchitecture: any\n' "$PKG" "$PKG" >"$SHLIBS_TMP/debian/control"
LIB_DEPENDS="$(cd "$SHLIBS_TMP" && dpkg-shlibdeps -l"$APPDIR" --ignore-missing-info \
    -O "${ELF_FILES[@]}" | sed -n 's/^shlibs:Depends=//p')"
rm -rf "$SHLIBS_TMP"
[ -n "$LIB_DEPENDS" ] || { echo "error: dpkg-shlibdeps produced no Depends." >&2; exit 1; }
DEPENDS="displayxr-runtime, $LIB_DEPENDS"
echo "==> Depends: $DEPENDS"

GLIBC_FLOOR="$(objdump -T "${ELF_FILES[@]}" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)"
GLIBCXX_FLOOR="$(objdump -T "${ELF_FILES[@]}" | grep -o 'GLIBCXX_[0-9.]*' | sed 's/GLIBCXX_//' | sort -uV | tail -1)"
echo "==> glibc floor: GLIBC_$GLIBC_FLOOR, GLIBCXX_$GLIBCXX_FLOOR"
if [ -n "${DXR_DEB_MAX_GLIBC:-}" ] &&
    [ "$(printf '%s\n%s\n' "$GLIBC_FLOOR" "$DXR_DEB_MAX_GLIBC" | sort -V | tail -1)" != "$DXR_DEB_MAX_GLIBC" ]; then
    echo "error: the binaries need GLIBC_$GLIBC_FLOOR, above DXR_DEB_MAX_GLIBC=$DXR_DEB_MAX_GLIBC" >&2
    echo "       (the oldest supported release). Build the .deb on that release —" >&2
    echo "       CI does this in an ubuntu:22.04 container." >&2
    exit 1
fi
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
