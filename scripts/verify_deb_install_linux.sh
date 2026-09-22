#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Install-verify a DisplayXR demo .deb on a CLEAN Ubuntu release. Run it as
# root inside a pristine ubuntu:<release> container — CI's DebInstall matrix
# does this for 22.04, 24.04 and 26.04; locally:
#
#   docker run --rm -v "$PWD:/w" -w /w ubuntu:22.04 \
#     ./scripts/verify_deb_install_linux.sh dist/<demo>_*_amd64.deb \
#                                           runtime-deb/displayxr-runtime_*_amd64.deb
#
# Fails if:
#   * apt cannot resolve the package's Depends from that release's archive
#     (installed with --no-install-recommends, so Depends alone must be enough:
#     a 24.04-only package name, or an unsatisfiable floor such as
#     `libc6 (>= 2.38)` on 22.04, fails here);
#   * a Depends / Recommends / Suggests name does not exist on that release;
#   * `ldd -r` on any installed ELF reports a missing library, symbol or symbol
#     version (that is what a glibc / libstdc++ floor above the release looks
#     like once the unversioned-Depends hole is closed);
#   * the launcher or the .desktop entry is missing;
#   * launching the demo dies in the dynamic loader. CI has no display or GPU,
#     so the app is EXPECTED to exit early — only loader-level failures
#     ("error while loading shared libraries", "symbol lookup error",
#     "version `GLIBC_2.xx' not found") are treated as failures. This is the
#     exact symptom the unversioned `libc6` used to produce on 22.04.
#   * `displayxr-cli selftest` fails: the demo Depends on displayxr-runtime, so
#     the runtime stack must come up on this release too (headless — no GPU,
#     window or display needed).
#
# The script is deliberately generic (package name, install dir, binary and
# launcher are all discovered from the .deb): the same file is copied verbatim
# into every demo repo, since the demos share no build-time mechanism.
set -euo pipefail

[ "$#" -ge 1 ] || { echo "usage: $0 <demo.deb> [displayxr-runtime.deb]" >&2; exit 2; }
[ "$(id -u)" = 0 ] || { echo "error: run as root in a throwaway container." >&2; exit 2; }

DEBS=()
for d in "$@"; do DEBS+=("$(readlink -f "$d")"); done
PKG="$(dpkg-deb -f "${DEBS[0]}" Package)"
. /etc/os-release
echo "==> $PRETTY_NAME: installing ${DEBS[*]##*/}"

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
# apt resolves each local .deb's Depends from this release's archive.
apt-get install -y -qq --no-install-recommends "${DEBS[@]}"

dpkg-query -W -f='==> installed ${Package} ${Version}\n    Depends: ${Depends}\n' "$PKG"

fail=0

# Every Depends / Recommends / Suggests alternative must name a real package
# here (apt already proved Depends; this also covers the optional fields).
for field in Depends Recommends Suggests; do
    for p in $(dpkg-query -W -f="\${$field}" "$PKG" | tr ',|' '\n\n' | sed 's/(.*)//; s/:any//; s/ //g' | sed '/^$/d'); do
        if apt-cache show "$p" >/dev/null 2>&1; then
            echo "    $field $p: available"
        else
            echo "error: $field '$p' does not exist on $PRETTY_NAME." >&2
            fail=1
        fi
    done
done

# ldd -r on every ELF the package installed (found by content, so a newly
# shipped binary or bundled library is covered without editing a list). The
# bundled OpenXR loader sits next to the binary and is found through the
# launcher's LD_LIBRARY_PATH, so reproduce that here.
elf_files() {
    local f
    for f in "$@"; do
        [ -f "$f" ] && [ ! -L "$f" ] || continue
        [ "$(head -c 4 "$f" | od -An -c | tr -d ' ')" = '177ELF' ] && echo "$f"
    done
}
mapfile -t ELFS < <(elf_files $(dpkg -L "$PKG"))
[ "${#ELFS[@]}" -ge 1 ] || { echo "error: no ELF files installed by $PKG." >&2; exit 1; }
for elf in "${ELFS[@]}"; do
    out="$(LD_LIBRARY_PATH="$(dirname "$elf")" ldd -r "$elf" 2>&1)" || true
    if grep -E 'not found|undefined symbol' <<<"$out"; then
        echo "error: unresolved dependency in $elf" >&2
        fail=1
    else
        echo "    ldd -r $elf: ok"
    fi
done

# Launcher + menu entry.
LAUNCHER="/usr/bin/$PKG"
[ -x "$LAUNCHER" ] || { echo "error: $LAUNCHER missing or not executable." >&2; fail=1; }
[ -f "/usr/share/applications/$PKG.desktop" ] || { echo "error: $PKG.desktop missing." >&2; fail=1; }

# Launch smoke: no display/GPU here, so the app is expected to fail — but it
# must get PAST the dynamic loader. A too-high glibc floor dies right here.
if [ -x "$LAUNCHER" ]; then
    echo "=== launch smoke (expected to exit: no display/GPU) ==="
    LOG="$(mktemp)"
    timeout 60 "$LAUNCHER" >"$LOG" 2>&1 || true
    sed 's/^/    /' "$LOG" | head -20
    if grep -E "error while loading shared libraries|symbol lookup error|version \`GLIBC[_0-9.]*' not found|version \`GLIBCXX[_0-9.]*' not found" "$LOG"; then
        echo "error: $PKG died in the dynamic loader on $PRETTY_NAME." >&2
        fail=1
    else
        echo "    no dynamic-loader failure"
    fi
    rm -f "$LOG"
fi

# The demo Depends on displayxr-runtime; prove the runtime stack runs here too.
if command -v displayxr-cli >/dev/null 2>&1; then
    echo "=== displayxr-cli selftest (env-free, sim-display) ==="
    unset XR_RUNTIME_JSON XRT_PLUGIN_SEARCH_PATH
    displayxr-cli info || fail=1
    displayxr-cli selftest || { echo "error: displayxr-cli selftest failed on $PRETTY_NAME." >&2; fail=1; }
fi

[ "$fail" = 0 ] || { echo "==> FAIL on $PRETTY_NAME" >&2; exit 1; }
echo "==> PASS on $PRETTY_NAME"
