#!/usr/bin/env bash
# Fetch the DisplayXR client wake library AAR pinned by android/build.gradle.
#
# The library ships as a release asset on the runtime repo (no Maven: the org's
# GitHub Packages budget is pinned at $0 with a hard stop, so publishing there
# would break every workflow in the org, not just this one).
#
#   ./android/fetch-client-aar.sh            # the pinned tag
#   ./android/fetch-client-aar.sh v2.16.29   # a specific tag
set -euo pipefail
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TAG="${1:-$(grep -oE "dxrClientRuntimeTag *= *'[^']+'" "$HERE/build.gradle" | grep -oE "v[0-9]+\.[0-9]+\.[0-9]+")}"
[ -n "$TAG" ] || { echo "Could not read dxrClientRuntimeTag from build.gradle" >&2; exit 1; }
mkdir -p "$HERE/libs"
# Remove older copies so the gradle resolver cannot pick a stale one.
rm -f "$HERE"/libs/DisplayXR-Client-*.aar

VER="${TAG#v}"
ASSET="DisplayXR-Client-${VER}.aar"

# gh when it is available and authenticated; otherwise a plain HTTPS download.
# The runtime repo is PUBLIC, so its release assets need no credential -- and CI
# here has only this repo's GITHUB_TOKEN, which is not a credential for another
# repository. Requiring gh auth would make the fallback path the common one.
if command -v gh >/dev/null && gh auth status >/dev/null 2>&1 \
   && gh release download "$TAG" -R DisplayXR/displayxr-runtime -p "$ASSET" -D "$HERE/libs" 2>/dev/null; then
    echo "fetched $ASSET via gh"
else
    URL="https://github.com/DisplayXR/displayxr-runtime/releases/download/${TAG}/${ASSET}"
    echo "fetching $URL"
    curl -fsSL --retry 3 -o "$HERE/libs/$ASSET" "$URL" \
      || { echo "Could not download $ASSET from $TAG." >&2
           echo "  Check that the release exists and carries that asset:" >&2
           echo "    gh release view $TAG -R DisplayXR/displayxr-runtime" >&2
           rm -f "$HERE/libs/$ASSET"; exit 1; }
fi
# An HTML error page saved as a .aar is the classic silent failure here: gradle
# would then fail with an unhelpful zip error far from the cause.
unzip -l "$HERE/libs/$ASSET" >/dev/null 2>&1 \
  || { echo "$ASSET is not a valid archive -- the download returned something else." >&2
       rm -f "$HERE/libs/$ASSET"; exit 1; }
ls -la "$HERE/libs"
