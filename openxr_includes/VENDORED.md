# Vendored DisplayXR OpenXR headers

This directory holds a **pinned copy** of the OpenXR core + DisplayXR extension
headers. This app is coupled to the runtime *only* through the OpenXR extension
wire protocol — it never `#include`s or links runtime-internal source. These
headers are that wire-protocol surface, vendored so the build never reaches back
into the runtime tree.

## Source

    https://github.com/DisplayXR/displayxr-runtime
    src/external/openxr_includes/openxr/

## Pins — `VENDORED.json` is the source of truth

`VENDORED.json` next to this file maps **every** vendored header to the full
40-char runtime commit at which the runtime's copy is **byte-identical** to ours.
`scripts/check_vendored_headers.py` re-derives that claim from the runtime and
`.github/workflows/lint.yml` runs it on every PR, so a hand-edited or silently
re-copied header fails CI instead of being discovered months later.

> **Why this file was rewritten (displayxr-demo-mediaplayer#61).** The pins used to
> be prose naming runtime commits at which the header path did not yet exist, so
> the provenance could not be checked at all. One header —
> `XR_DXR_xlib_window_binding.h` — turned out to match **no** runtime commit: it
> was a hand-merge of the post-rename file with the later `transparentBackground`
> field bolted on. It has been re-copied verbatim from the runtime (the only
> difference was two lines of comment text; `SPEC_VERSION` and the struct were
> already identical).

Pins in force:

| Runtime commit | Headers |
|---|---|
| `220e9393511aab23c1ef2c6bb796d452f4fe3060`<br>220e93935 (2026-09-07) feat(android): XR_DXR_android_surface_binding v2 — mini-window layout hint (#1396) (#1398) | `XR_DXR_depth_budget.h`, `XR_DXR_display_zones.h`, `XR_DXR_xlib_window_binding.h`, `XR_MNDX_ball_on_a_stick_controller.h`, `XR_MNDX_blubur_s1.h`, `XR_MNDX_hydra.h`, `XR_MNDX_oculus_remote.h`, `XR_MNDX_system_buttons.h`, `XR_MNDX_xdev_space.h`, `openxr.h`, `openxr_extension_helpers.h`, `openxr_loader_negotiation.h`, `openxr_platform.h`, `openxr_platform_defines.h`, `openxr_reflection.h`, `openxr_reflection_parent_structs.h`, `openxr_reflection_structs.h` |
| `a71979a4d1385841a224eccd64ae973385300b1f`<br>a71979a4d (2026-07-12) feat(#734): fold planned XR_EXT_android_surface_binding → XR_DXR_ (docs/comments); post-rename-safe map regen | `XR_DXR_atlas_capture.h`, `XR_DXR_cocoa_window_binding.h`, `XR_DXR_display_info.h`, `XR_DXR_local_3d_zone.h`, `XR_DXR_macos_gl_binding.h`, `XR_DXR_mcp_tools.h`, `XR_DXR_spatial_workspace.h`, `XR_DXR_view_rig.h`, `XR_DXR_weave.h`, `XR_DXR_win32_window_binding.h`, `XR_DXR_workspace_file_dialog.h` |
| `344825131c2a7443a3916e760be0942f2f1f0732`<br>344825131 (2026-09-06) feat(depth-budget): v3 header draft — XrContentMaskDXR occupancy mask (#1365) | `XR_DXR_android_surface_binding.h` |

## Known drift vs runtime `main`

Deliberately **not** brought forward in the #61 pass: re-copying unrelated
extension revisions into a provenance fix is exactly what should not ride
along. Everything below is additive on the runtime side — no struct this app
passes over the wire changed shape, so the app is correct as pinned.

| Header | What is newer on runtime `main` |
|---|---|
| `XR_DXR_android_surface_binding.h` | spec v1 here; runtime main is v2, which adds `XrEventDataAndroidWindowLayoutHintDXR` (mini-window layout hint, runtime#1396/#1398, first shipped in runtime v2.16.17). Additive event struct. |
| `XR_DXR_atlas_capture.h` | SPEC_VERSION macro is 1 here; runtime main carries the real number. The demos' copies date from the ~24h window between the `XR_EXT_* → XR_DXR_*` rename (runtime `fefa3d3dc`/`a71979a4d`, 2026-07-12) and `2a87861e2`, which restored the pre-rename SPEC_VERSION values. No struct or enum change. |
| `XR_DXR_cocoa_window_binding.h` | SPEC_VERSION macro is 1 here; runtime main carries the real number. The demos' copies date from the ~24h window between the `XR_EXT_* → XR_DXR_*` rename (runtime `fefa3d3dc`/`a71979a4d`, 2026-07-12) and `2a87861e2`, which restored the pre-rename SPEC_VERSION values. No struct or enum change. |
| `XR_DXR_display_info.h` | runtime main is spec v18: adds `XrDisplayDesktopInfoDXR` (panel desktop rect + device name, runtime#1301/#1317). Purely ADDITIVE — a NEW chained struct; `XrDisplayInfoDXR` itself is byte-identical. Nothing this app reads changed. |
| `XR_DXR_local_3d_zone.h` | SPEC_VERSION macro is 1 here; runtime main carries the real number. The demos' copies date from the ~24h window between the `XR_EXT_* → XR_DXR_*` rename (runtime `fefa3d3dc`/`a71979a4d`, 2026-07-12) and `2a87861e2`, which restored the pre-rename SPEC_VERSION values. No struct or enum change. |
| `XR_DXR_macos_gl_binding.h` | SPEC_VERSION macro is 1 here; runtime main carries the real number. The demos' copies date from the ~24h window between the `XR_EXT_* → XR_DXR_*` rename (runtime `fefa3d3dc`/`a71979a4d`, 2026-07-12) and `2a87861e2`, which restored the pre-rename SPEC_VERSION values. No struct or enum change. |
| `XR_DXR_mcp_tools.h` | SPEC_VERSION macro is 1 here; runtime main carries the real number. The demos' copies date from the ~24h window between the `XR_EXT_* → XR_DXR_*` rename (runtime `fefa3d3dc`/`a71979a4d`, 2026-07-12) and `2a87861e2`, which restored the pre-rename SPEC_VERSION values. No struct or enum change. |
| `XR_DXR_spatial_workspace.h` | SPEC_VERSION macro is 1 here; runtime main carries the real number. The demos' copies date from the ~24h window between the `XR_EXT_* → XR_DXR_*` rename (runtime `fefa3d3dc`/`a71979a4d`, 2026-07-12) and `2a87861e2`, which restored the pre-rename SPEC_VERSION values. No struct or enum change. |
| `XR_DXR_view_rig.h` | SPEC_VERSION macro is 1 here; runtime main carries the real number. The demos' copies date from the ~24h window between the `XR_EXT_* → XR_DXR_*` rename (runtime `fefa3d3dc`/`a71979a4d`, 2026-07-12) and `2a87861e2`, which restored the pre-rename SPEC_VERSION values. No struct or enum change. |
| `XR_DXR_weave.h` | runtime main is spec v8 (batched submit, overlay atlas, N-view worst-case atlas, platform-neutral handle kinds). Browser/inline-3D surface — not used by this app. |
| `XR_DXR_win32_window_binding.h` | SPEC_VERSION 1 here vs 8 on runtime main; the struct is unchanged (the only text delta is dropping the retired chroma-key sentence). |
| `XR_DXR_workspace_file_dialog.h` | SPEC_VERSION macro is 1 here; runtime main carries the real number. The demos' copies date from the ~24h window between the `XR_EXT_* → XR_DXR_*` rename (runtime `fefa3d3dc`/`a71979a4d`, 2026-07-12) and `2a87861e2`, which restored the pre-rename SPEC_VERSION values. No struct or enum change. |

`scripts/check_vendored_headers.py --drift` prints this list live (it is
informational — only a pin *mismatch* fails CI).

## Updating

1. Re-copy the file(s) verbatim from a runtime checkout — never edit in place.
2. Update that file's entry in `VENDORED.json` to the runtime commit you copied from.
3. `python3 scripts/check_vendored_headers.py --drift` (or `--runtime ../displayxr-runtime` offline).
4. Note anything non-additive in **Known drift** above, and bump the
   `FetchContent` OpenXR `GIT_TAG` if the core headers moved.

Refreshed in the #61 pass: `XR_DXR_xlib_window_binding.h`.
