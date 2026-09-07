#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
"""Prove every vendored OpenXR header is byte-identical to its pinned runtime commit.

Why this exists (displayxr-demo-mediaplayer#61)
----------------------------------------------
This repo vendors the DisplayXR extension headers -- the ONLY coupling to the
runtime is the OpenXR extension wire protocol, and these headers are that
surface.  A vendored copy is only trustworthy if you can say *which* runtime
commit it came from and check that claim.  The pins used to live in prose and
named commits at which the header path did not yet exist, so the provenance was
unverifiable: the copy could have been hand-edited (it had been -- see
``XR_DXR_xlib_window_binding.h``) and nothing would have noticed.

``VENDORED.json`` next to the headers is now the machine-readable pin table:
one full 40-char runtime commit SHA per file, chosen as the NEWEST commit at
which the runtime's copy is byte-identical to ours.  This script re-derives
that claim from the runtime itself.

Two failure modes it catches
----------------------------
* **Tampered / stale copy** -- the local bytes no longer match the pin.  Either
  the header was edited in place (never do that) or someone re-copied without
  re-pinning.
* **Unpinned file** -- a header appears in the vendor directory with no entry in
  ``VENDORED.json``.  Adding a header without recording where it came from is
  how the unverifiable state happened in the first place.

Drift against runtime ``main`` is reported by ``--drift`` and is NOT a failure:
being deliberately behind a newer extension revision is a normal, documented
state (VENDORED.md records it).  The failure is not *knowing*.

Usage
-----
    scripts/check_vendored_headers.py                 # verify against the pins (network)
    scripts/check_vendored_headers.py --drift         # + report what is behind runtime main
    scripts/check_vendored_headers.py --runtime ../displayxr-runtime   # offline, local clone
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request

RAW = "https://raw.githubusercontent.com/{repo}/{ref}/{path}"
API_TIMEOUT = 30


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def fetch_remote(repo, ref, path):
    """Bytes of `path` at `ref` from GitHub, or None if it does not exist there."""
    url = RAW.format(repo=repo, ref=ref, path=path)
    req = urllib.request.Request(url, headers={"User-Agent": "displayxr-vendored-header-check"})
    try:
        with urllib.request.urlopen(req, timeout=API_TIMEOUT) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise


def fetch_local(repo_dir, ref, path):
    """Bytes of `path` at `ref` from a local clone, or None if absent there."""
    try:
        return subprocess.check_output(
            ["git", "show", "%s:%s" % (ref, path)], cwd=repo_dir, stderr=subprocess.DEVNULL
        )
    except subprocess.CalledProcessError:
        return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manifest", default=None,
                    help="path to VENDORED.json (default: found relative to this script)")
    ap.add_argument("--runtime", default=os.environ.get("DXR_RUNTIME_DIR"),
                    help="local displayxr-runtime clone to read instead of GitHub "
                         "(env: DXR_RUNTIME_DIR)")
    ap.add_argument("--drift", action="store_true",
                    help="also report headers that differ from runtime main (never fatal)")
    args = ap.parse_args(argv)

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    manifest_path = args.manifest
    if manifest_path is None:
        for cand in ("third_party/displayxr-openxr/VENDORED.json", "openxr_includes/VENDORED.json"):
            p = os.path.join(root, cand)
            if os.path.exists(p):
                manifest_path = p
                break
    if manifest_path is None or not os.path.exists(manifest_path):
        print("error: no VENDORED.json found -- pass --manifest", file=sys.stderr)
        return 2

    man = json.load(open(manifest_path))
    repo = man["source_repo"]
    src_path = man["source_path"].rstrip("/") + "/"
    vendor_dir = os.path.join(root, man["vendor_dir"])
    pins = man["pins"]

    if not os.path.isdir(vendor_dir):
        print("error: vendor dir %s does not exist" % vendor_dir, file=sys.stderr)
        return 2

    get = (lambda ref, p: fetch_local(args.runtime, ref, p)) if args.runtime else \
          (lambda ref, p: fetch_remote(repo, ref, p))
    where = args.runtime if args.runtime else "https://github.com/%s" % repo
    print("vendor dir : %s" % man["vendor_dir"])
    print("runtime    : %s  (%s)" % (where, src_path))

    on_disk = sorted(f for f in os.listdir(vendor_dir) if f.endswith(".h"))
    bad = []
    unpinned = [f for f in on_disk if f not in pins]
    stale_pins = [f for f in pins if f not in on_disk]

    for f in on_disk:
        if f not in pins:
            continue
        pin = pins[f]
        if len(pin) != 40 or not all(c in "0123456789abcdef" for c in pin):
            print("  FAIL %-42s pin %r is not a full 40-char commit SHA" % (f, pin))
            bad.append(f)
            continue
        local = open(os.path.join(vendor_dir, f), "rb").read()
        remote = get(pin, src_path + f)
        if remote is None:
            print("  FAIL %-42s %s: the runtime has no %s at that commit "
                  "(this is exactly the unverifiable-pin bug, #61)" % (f, pin[:9], src_path + f))
            bad.append(f)
            continue
        if sha256(local) != sha256(remote):
            print("  FAIL %-42s differs from %s (local sha256 %s, runtime %s)"
                  % (f, pin[:9], sha256(local)[:12], sha256(remote)[:12]))
            bad.append(f)
        else:
            print("  ok   %-42s == %s" % (f, pin[:9]))

    rc = 0
    if unpinned:
        print("\n::error::vendored header(s) with no VENDORED.json pin: %s" % ", ".join(unpinned))
        print("Every vendored header must record the runtime commit it was copied from.")
        rc = 1
    if stale_pins:
        print("\n::error::VENDORED.json pins a file that is not in the vendor dir: %s"
              % ", ".join(stale_pins))
        rc = 1
    if bad:
        print("\n::error::%d vendored header(s) do not match their pinned runtime commit: %s"
              % (len(bad), ", ".join(bad)))
        print("Do NOT edit vendored headers in place. Re-copy from the runtime and re-pin "
              "VENDORED.json + VENDORED.md.")
        rc = 1

    if args.drift:
        print("\n--- drift vs runtime main (informational; being behind is a documented state) ---")
        behind = []
        for f in on_disk:
            local = open(os.path.join(vendor_dir, f), "rb").read()
            remote = get("main", src_path + f)
            if remote is None:
                print("  gone  %-42s no longer exists in runtime main" % f)
                continue
            if sha256(local) != sha256(remote):
                behind.append(f)
                print("  BEHIND %-41s differs from runtime main" % f)
        if not behind:
            print("  all vendored headers are current with runtime main")
        else:
            print("  %d header(s) behind runtime main -- see VENDORED.md 'Known drift'" % len(behind))

    if rc == 0:
        print("\nOK: every vendored header is byte-identical to its pinned runtime commit.")
    return rc


if __name__ == "__main__":
    sys.exit(main())
