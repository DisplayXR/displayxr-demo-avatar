#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
"""Android versionCode / versionName arithmetic for this demo's APK.

This is the HOST-SIDE TWIN of the closures in ``android/build.gradle``
(``getVersionCode`` / ``getVersionString``).  Gradle owns the value that goes
into the APK; this script owns the value CI expects to find there.  Two
independent implementations of one rule is the point -- CI compares what gradle
actually stamped into the built APK against what this computes, so a silent
change to either side fails the build instead of shipping.

Why this exists (displayxr-demo-mediaplayer#60)
----------------------------------------------
Every DisplayXR demo APK shipped ``versionCode=1`` / ``versionName="0.1"``,
hardcoded in ``android/build.gradle``.  Consequences, all observed:

* No on-device way to tell which demo release is installed.  The Android bundle
  harness had to derive the version from the bundle FILENAME and prove the
  installed APK is that file by sha256, because the APK's own stamps say
  nothing.
* A version-gated test that keys on the installed ``versionName`` reads
  ``0.1``, concludes "old app", and passes green while proving nothing.
* ``versionCode`` never increases, so a sideload of an OLDER build over a newer
  one is not rejected -- ``adb install -r`` works only because the signature
  matches.

The scheme is the runtime's (runtime#1379 / runtime#1384,
``scripts/android_version_code.py`` there), adopted verbatim so every DisplayXR
APK on a device encodes its version the same way.

The scheme
----------
Positional arithmetic, not string concatenation::

    code = major * 100_000_000     # <= 20   (weight 1e8)
         + minor *   1_000_000     # <= 99   (weight 1e6)
         + patch *      10_000     # <= 99   (weight 1e4)
         + commits                 # <= 9999 (weight 1e0)

Every field is strictly narrower than its weight, so ordering by ``code`` is
exactly lexicographic ordering by ``(major, minor, patch, commits)``.
``v20.99.99-9999`` = 2,099,999,999 <= ``Integer.MAX_VALUE`` (2,147,483,647) and
also under Google Play's 2,100,000,000 ceiling.

Two traps this encoding exists to avoid, both of which BIT THE RUNTIME:

* ``git describe`` WITHOUT ``--tags`` only considers ANNOTATED tags.  DisplayXR
  release tags are lightweight (plain ``git tag``), so describe silently names
  the last annotated tag -- the runtime shipped every APK from v2.14.5 to
  v2.16.14 calling itself ``v2.14.5-<n>-g<sha>``.  The fix is ``--tags``.
  NEVER "fix" it by annotating the tags.
* The runtime's old ``"%02d%01d%01d%05d"`` string format used MINIMUM field
  widths, so a two-digit minor/patch widened the string past int32,
  ``Integer.parseInt`` threw, and a ``catch (ignored) { return null }`` shipped
  an APK with an EMPTY versionCode -- Android then cannot compare versions at
  all and no installed unit can upgrade in place.  Hence: arithmetic, hard
  field-width guards, and no silent fallback anywhere.

Shipped floor
-------------
Every demo APK in the field today carries ``versionCode=1``, so ANY code this
scheme produces (minimum ``v0.0.1-0`` = 10,000) is an upgrade.  ``SHIPPED_FLOOR``
is 1 and CI asserts the built APK beats it.

Usage
-----
    android_version_code.py --selftest
    android_version_code.py --describe v1.9.3-4-gc8c2964
    android_version_code.py --git /path/to/repo          # runs git describe
    android_version_code.py --git . --print name         # versionName instead
"""

import argparse
import re
import subprocess
import sys

# Integer.MAX_VALUE -- Android's versionCode is a signed 32-bit int.
INT32_MAX = 2147483647
# Google Play's own (lower) ceiling.
PLAY_CAP = 2100000000
# Highest versionCode ever SHIPPED by a demo APK: the hardcoded 1 (#60).
SHIPPED_FLOOR = 1

# Field weights and inclusive maxima, in most-significant-first order.
FIELDS = (
    ("major", 100000000, 20),
    ("minor", 1000000, 99),
    ("patch", 10000, 99),
    ("commits", 1, 9999),
)

# `git describe --tags --long --match 'v[0-9]*'` output.
DESCRIBE_LONG_RE = re.compile(
    r"^v(?P<major>\d+)\.(?P<minor>\d+)\.(?P<patch>\d+)-(?P<commits>\d+)-g(?P<hash>[0-9a-f]+)(?P<dirty>-dirty)?$"
)


class VersionCodeError(ValueError):
    """Raised instead of returning a wrong/None code -- never swallow this."""


def parse_describe(describe):
    """Parse `git describe --tags --long` output into (major, minor, patch, commits)."""
    m = DESCRIBE_LONG_RE.match(describe.strip())
    if not m:
        raise VersionCodeError(
            "%r is not vMAJOR.MINOR.PATCH-<commits>-g<sha> -- "
            "did you forget `git describe --tags --long --match 'v[0-9]*'`?" % describe
        )
    return tuple(int(m.group(g)) for g in ("major", "minor", "patch", "commits"))


def version_code(major, minor, patch, commits):
    """Compute the Android versionCode. Raises VersionCodeError; never returns None."""
    values = dict(major=major, minor=minor, patch=patch, commits=commits)
    for name, _weight, cap in FIELDS:
        v = values[name]
        if v < 0 or v > cap:
            raise VersionCodeError(
                "%s=%d is outside 0..%d -- the positional versionCode encoding "
                "would collapse and stop being monotonic" % (name, v, cap)
            )
    code = sum(values[name] * weight for name, weight, _cap in FIELDS)
    if code > INT32_MAX:
        raise VersionCodeError(
            "versionCode %d exceeds Integer.MAX_VALUE (%d)" % (code, INT32_MAX)
        )
    return code


def version_code_from_describe(describe):
    return version_code(*parse_describe(describe))


def version_name_from_describe(describe):
    """versionName as gradle emits it: 'vX.Y.Z' on the tag, 'vX.Y.Z-N-gsha' past it.

    Gradle gets this straight from `git describe --tags --dirty` (no --long);
    this reconstructs the same string from the --long form so CI needs only one
    git invocation.
    """
    m = DESCRIBE_LONG_RE.match(describe.strip())
    if not m:
        raise VersionCodeError("%r is not a --long describe" % describe)
    base = "v%s.%s.%s" % (m.group("major"), m.group("minor"), m.group("patch"))
    if int(m.group("commits")) != 0:
        base += "-%s-g%s" % (m.group("commits"), m.group("hash"))
    if m.group("dirty"):
        base += "-dirty"
    return base


def git_describe_long(repo):
    out = subprocess.check_output(
        ["git", "describe", "--tags", "--long", "--dirty", "--match", "v[0-9]*"],
        cwd=repo,
        text=True,
    ).strip()
    if not out:
        raise VersionCodeError("git describe produced no output in %s" % repo)
    return out


def selftest():
    ok = True

    def check(label, got, want):
        nonlocal ok
        if got != want:
            print("FAIL %-56s got %r want %r" % (label, got, want))
            ok = False
        else:
            print("ok   %-56s %r" % (label, got))

    def raises(label, fn):
        nonlocal ok
        try:
            fn()
        except VersionCodeError:
            print("ok   %-56s raised" % label)
            return
        print("FAIL %-56s did NOT raise" % label)
        ok = False

    # --- worked values across the demo version series ----------------------
    # The demos span v0.x (avatar, earthview, modelviewer) and v1.x
    # (mediaplayer, gaussiansplat), so both shapes are pinned here.
    check("v0.7.6-0    (earthview on the tag)",
          version_code_from_describe("v0.7.6-0-g8022d33"), 7060000)
    check("v0.11.7-0   (avatar on the tag)",
          version_code_from_describe("v0.11.7-0-g1ab96f4"), 11070000)
    check("v0.28.3-0   (modelviewer on the tag)",
          version_code_from_describe("v0.28.3-0-g33a3931"), 28030000)
    check("v1.9.3-0    (mediaplayer on the tag)",
          version_code_from_describe("v1.9.3-0-gc8c2964"), 109030000)
    check("v1.25.3-0   (gaussiansplat on the tag)",
          version_code_from_describe("v1.25.3-0-g0b85772"), 125030000)

    # --- monotonicity vs what is installed today (the hardcoded 1) ---------
    check("shipped floor is the hardcoded versionCode 1", SHIPPED_FLOOR, 1)
    for d in ("v0.0.1-0-ga", "v0.7.6-0-ga", "v1.9.3-0-ga", "v1.25.3-4-ga"):
        check("%s beats the shipped floor" % d,
              version_code_from_describe(d) > SHIPPED_FLOOR, True)

    # --- strict ordering across a realistic release ladder -----------------
    ladder = [
        "v0.7.6-0-ga", "v0.7.6-1-ga", "v0.7.6-291-ga",
        "v0.8.0-0-ga", "v0.28.3-0-ga", "v0.99.99-9999-ga",
        "v1.0.0-0-ga", "v1.9.3-0-ga", "v1.25.3-0-ga",
        "v1.99.99-9999-ga", "v2.0.0-0-ga", "v20.99.99-9999-ga",
    ]
    codes = [version_code_from_describe(d) for d in ladder]
    check("ladder is strictly increasing",
          all(a < b for a, b in zip(codes, codes[1:])), True)

    # --- headroom ----------------------------------------------------------
    check("v0.99.99-9999 (worst v0) = 99,999,999",
          version_code_from_describe("v0.99.99-9999-ga"), 99999999)
    check("v1.0.0 beats every v0",
          version_code_from_describe("v1.0.0-0-ga") >
          version_code_from_describe("v0.99.99-9999-ga"), True)
    check("v20.99.99-9999 (absolute max) fits int32",
          version_code_from_describe("v20.99.99-9999-ga"), 2099999999)
    check("absolute max is under Google Play's cap",
          version_code_from_describe("v20.99.99-9999-ga") <= PLAY_CAP, True)

    # --- the traps that must FAIL LOUDLY, never return None ----------------
    raises("major 21 overflows the encoding",
           lambda: version_code_from_describe("v21.0.0-0-ga"))
    raises("minor 100 breaks the field width",
           lambda: version_code_from_describe("v0.100.0-0-ga"))
    raises("patch 100 breaks the field width",
           lambda: version_code_from_describe("v0.0.100-0-ga"))
    raises("commits 10000 breaks the field width",
           lambda: version_code_from_describe("v0.0.0-10000-ga"))
    raises("a bare SHA (tagless/shallow clone) is rejected",
           lambda: version_code_from_describe("c8c2964"))
    raises("empty describe is rejected",
           lambda: version_code_from_describe(""))

    # --- the runtime's OLD string format, reproduced, to show why ----------
    # arithmetic replaced it: %01d is a MINIMUM width, so two-digit fields
    # widen the string and overflow int32.
    old = "%02d%01d%01d%05d" % (2, 16, 12, 291)
    check("old string format for v2.16.12-291 is 11 digits", old, "02161200291")
    check("old string format overflows int32", int(old) > INT32_MAX, True)

    # --- versionName -------------------------------------------------------
    check("versionName on the tag",
          version_name_from_describe("v1.9.3-0-gc8c2964"), "v1.9.3")
    check("versionName past the tag",
          version_name_from_describe("v1.9.3-4-gc8c2964"), "v1.9.3-4-gc8c2964")
    check("versionName keeps -dirty",
          version_name_from_describe("v1.9.3-4-gc8c2964-dirty"), "v1.9.3-4-gc8c2964-dirty")

    print("\n%s" % ("SELFTEST PASSED" if ok else "SELFTEST FAILED"))
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--selftest", action="store_true", help="run the unit tests and exit")
    g.add_argument("--describe", metavar="STR", help="a `git describe --tags --long` string")
    g.add_argument("--git", metavar="REPO", help="run git describe in REPO")
    ap.add_argument("--print", dest="what", choices=("code", "name", "both"), default="code",
                    help="what to print (default: code)")
    ap.add_argument("--floor", type=int, default=None,
                    help="fail if the computed code is <= FLOOR (use %d for the shipped floor)"
                         % SHIPPED_FLOOR)
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    describe = args.describe if args.describe else git_describe_long(args.git)
    try:
        code = version_code_from_describe(describe)
        name = version_name_from_describe(describe)
    except VersionCodeError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2

    if args.floor is not None and code <= args.floor:
        print("error: versionCode %d is not above the floor %d -- installed units "
              "would refuse it as a downgrade" % (code, args.floor), file=sys.stderr)
        return 3

    if args.what == "code":
        print(code)
    elif args.what == "name":
        print(name)
    else:
        print("%s %d" % (name, code))
    return 0


if __name__ == "__main__":
    sys.exit(main())
