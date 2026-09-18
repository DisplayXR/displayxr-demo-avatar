# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working on this repo.

## Project Overview

DisplayXR Demo — **3D Avatar**. A transparent, click-through 3D character
("**Leo**", an animated tiger) that floats over the desktop on a glasses-free 3D
display, on the DisplayXR OpenXR runtime via Vulkan. A native-Vulkan `_handle`
OpenXR app **derived from `displayxr-demo-modelviewer`**: it reuses that demo's
`model_common/` renderer to weave the bundled FBX, then wraps it in an avatar
shell — transparent-by-default borderless window, per-pixel click-through, a
face-the-viewer billboard, depth dolly with display-plane clipping, and a flat
2D speech bubble (`XR_DXR_local_3d_zone` Local2D) above the weaved character.
Standalone repo, independent release cadence; `common/` + `openxr_includes/`
were seeded from the runtime and are maintained here.

**Status: Windows working; macOS port in progress.** The renderer is fully
implemented (inherited from modelviewer — NOT a skeleton). Bundled avatar =
`assets/tiger/avatar.fbx` (+ `rgb.jpg`), copied next to the exe and auto-loaded
at startup.

### Avatar shell (what this demo adds on top of the modelviewer renderer)
- **Transparent-by-default window** — borderless `WS_POPUP` + `WS_EX_TOPMOST` +
  `WS_EX_NOREDIRECTIONBITMAP`; session transparency is wired at `xrCreateSession`
  (always on). `g_transparentBg` flips the renderer's output alpha + enables
  display-plane foreground clipping. `Ctrl+T` toggles for debugging. `B` toggles
  decoration (title bar) so the OS can move/resize, then back to borderless.
- **Per-pixel click-through** — `UpdateSilhouette` renders the FIRST and LAST
  views into two scratch images **every frame** (same frame → the union is
  self-consistent), reads back alpha through a `HOST_CACHED` pipelined readback
  → thresholded + 1-texel-dilated coverage bitmap; `UpdateClickRegion` shapes
  the window with `SetWindowRgn` so input outside the silhouette falls through
  cross-process. **The region is a VISUAL CLIP, not just a hit mask** — outside
  it the window is not drawn — so the raster scales with the window
  (4 px/texel, capped 1024×576) and errs outward. Levers:
  `DXR_AVATAR_SIL_TEXEL_PX` / `_DILATE` / `_ALPHA`, `DXR_VK_NO_HOST_CACHED=1`.
  `DXR_DUMP_SILHOUETTE=1` dumps the mask to `%TEMP%\avatar_silhouette.png`.
- **Two silhouettes, deliberately.** The click-through region uses the RENDERED
  (post-far-clip) alpha — it is a visual clip, so it must. The
  `XR_DXR_depth_budget` v3 `XrContentMaskDXR` must NOT: `pbr.frag`'s far discard
  would make the mask a function of the budget the runtime published, and the
  two oscillate (~0.6–1.1 s open/close, runtime#1470). So the mask comes from
  `ModelRenderer::beginContentMaskFrame()` / `contentMaskCoverage()` — a
  coverage-only re-draw (same `pbr.vert`, same skinning, empty
  `shaders/coverage.frag`) into a 256×144 R8 target, armed around the silhouette
  pass's two views and read back one frame later in `beginFrame()`. The v2
  `XrContentBoundsDXR` bounds were always clip-independent (a projected
  model-space AABB) and stay chained as the runtime's fallback.
  **TWO clips, not one (runtime#1470).** Removing `pbr.frag`'s far discard was
  only half of it: the coverage pass reused the view's `UniformBlock`, whose
  `viewProj` carries the rear-budget far plane, so the rasterizer's NDC z > 1
  clip still cut the rear half before a fragment ran and the mask stayed
  budget-dependent. The pass therefore gets its OWN uniform slot, written from
  an **unrestricted** projection (same fov, same near, far =
  `dxr::ResolveClipPlanes` at `farOffsetVH = 1000`) — `renderEye`'s
  `MaskProjections` argument. Never use `depthClampEnable` for this.
  `DXR_AVATAR_MASKPASS_TEST=1` arms the pass on every platform and logs a
  covered-texel count + one ASCII silhouette dump, so the Vulkan objects, the
  draw and the flip are exercisable on macOS/MoltenVK where nothing chains a
  mask. Adding `DXR_AVATAR_MASKPASS_MUTATE_VH=<offset in vH>` (`1` = the -0.1
  default) makes it a **mutation test**: the REAL pass gets an artificially
  restricted far plane that slices the character, and the renderer records two
  extra coverage probes in the same frame — the real-pass projection (the
  pre-fix source) and that rig at far = unrestricted — then asserts
  `unrestricted == real-unrestricted` and `real-restricted < real-unrestricted`.
  The offset must be NEGATIVE to prove anything: the avatar is shallow next to
  vH, so +0.05 vH cuts ~0.1 % of the silhouette and on some animation frames
  exactly nothing.
- **Face-the-viewer billboard** — yaw tracks the tracked head centre (from
  `rawEyes`), time-based smoothing, gated on `xr->isEyeTracking`. Pitch is
  implemented but disabled (`FACE_PITCH_SIGN`). LMB-drag overrides it.
- **Bottom-70 % confinement + speech bubble** — the Kooima canvas is a bottom-70 %
  sub-rect (full width, centre at 65 %) and the avatar renders into the bottom-70 %
  sub-viewport of each tile; the top 30 % is a flat 2D bubble submitted by a
  hand-built `xrEndFrame` (a `XrCompositionLayerLocal2DDXR` pill + a full-band
  transparent backer, both implicit-M=0). This is the handle-app equivalent of a
  texture app's `xrSetSharedTextureOutputRectDXR`.
- **W/S depth dolly only** — X/Y are anchored each frame to the animated skeleton
  centroid (`getAnimatedAnchor`), so A/D/Q/E are effectively no-ops; `W`/`S`
  drive Z through the display-plane clip.

## Runtime dependency

Requires **DisplayXR runtime ≥ v1.9.1** (v1.9.1 has the per-app VK-native
compositor in-place DComp resize fix this demo relies on for clean window
resizing). Install `DisplayXRSetup-*.exe` from the
[runtime releases](https://github.com/DisplayXR/displayxr-runtime/releases).
Apps load the runtime via the registry-resolved manifest (no `XR_RUNTIME_JSON`).

## Architecture

```
windows/main.cpp                 — platform entry + avatar shell: borderless
                                    transparent window, OpenXR session, per-frame
                                    view/projection (bottom-70% Kooima sub-canvas),
                                    billboard, click-through silhouette + window
                                    region, speech-bubble Local2D xrEndFrame, atlas
                                    capture ('I'). Auto-loads the bundled tiger;
                                    swap the character via Ctrl+O, drag-drop, a
                                    CLI model-path arg, or the load_model MCP
                                    tool. All four funnel through LoadAvatarModel
                                    (validate → g_sceneMutex swap → auto-fit).
                                    A drop only registers where the click-through
                                    region exists — the silhouette + bubble when
                                    undecorated, the whole window after B.
windows/xr_session.cpp/.h        — OpenXR instance/session/Vulkan setup; enables
                                    XR_DXR_local_3d_zone (g_hasLocal3DZone gates
                                    the bubble).
macos/main.mm                    — Cocoa/MoltenVK avatar-shell port (in progress).
model_common/                     — the renderer (vendor-neutral, analog of
                                    3dgs_common in the gaussiansplat demo):
  model_loader.{h,cpp}            — format dispatcher (by extension) + path
                                    helpers; fills ModelData = interleaved verts
                                    + indices + materials + decoded RGBA textures
                                    + node-baked world transforms + AABB
  model_loader_gltf.cpp           — .glb/.gltf backend (tinygltf); the others
  model_loader_{stl,obj,fbx,usd}.cpp  feed the SAME ModelData (renderer is
  model_loader_material.{h,cpp}   — format-neutral). OBJ/FBX share this texture
                                    + Phong→roughness shim. third_party/ vendors
                                    tinyobjloader + ufbx; tinyusdz is FetchContent
  model_renderer.{h,cpp}          — metallic-roughness GGX raster pass into an
                                    internal colour image, blitted into the
                                    per-eye swapchain viewport; IBL generation
  model_vulkan_utils.{h,cpp}      — VkBuffer/VkImage helpers
  shaders/                        — pbr.{vert,frag}; skybox.frag; IBL gen:
                                    fullscreen.vert, brdf_lut/irradiance/
                                    prefilter.frag, sky.glsl + ibl_common.glsl
common/                           — Kooima view math (display3d_view.*),
                                    camera3d_view (unused), input, HUD, stb;
                                    dxr_view_config.h (vendored #1486 helper) +
                                    dxr_submit_views.h (submit clamp), shared by
                                    ALL FOUR legs
openxr_includes/                  — vendored OpenXR + DisplayXR ext headers
```

### View configuration — this is an N-view app on every leg (runtime #1486/#1500)
`XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO` reports **exactly 2** views and
`xrEndFrame` rejects a projection layer that carries more, so an app whose
per-frame view count comes from the active DXR rendering mode must begin its
session with `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR` — otherwise it
goes black in sim_display's 4-view Quad mode (reachable with V / 4 / 5 on any dev
box). Every leg therefore calls **`DxrSelectViewConfigType(instance, systemId)`
once, right after `xrGetSystem` and before the first view-configuration-typed
call**, and feeds that one variable to `xrEnumerateViewConfigurationViews`,
`XrSessionBeginInfo::primaryViewConfigurationType` and
`XrViewLocateInfo::viewConfigurationType` (Windows: `xr.viewConfigType` on
displayxr-common's `XrSessionManager`, set in `windows/xr_session.cpp`; macOS /
Linux: the same field on their own `AppXrSession`; Android:
`g_view_config_type`). It degrades to `PRIMARY_STEREO` on an older runtime, so
it is unconditional.

**The count that reaches `xrEndFrame` comes from the render path, never from the
mode.** `DxrClampSubmitViewCount` (common/dxr_submit_views.h) returns
`min(active mode count, located count, atlas tile capacity)` and logs once when
the three disagree; 0 means submit no projection layer at all. Recomputing the
mode's count at submit time is what #1500 turned from "looks wrong" into "fails
`xrEndFrame`, which wedges the session" — a rejected frame is never ended, so
every later `xrBeginFrame` returns `XR_FRAME_DISCARDED`. The Linux leg is
stereo-fixed (2 tiles, no mode enumeration) and opts in anyway for uniformity;
the clamp is what actually protects it.

**ADOPT the runtime's active mode; never force one at startup.** Every leg
reads `XrDisplayRenderingModeInfoDXR::isActive` (display_info v13) once after
`xrCreateSession` and takes it; the log line is `Startup rendering mode: N (…)`
on Windows and `Adopting runtime's active rendering mode: N` on macOS/Linux.
macOS used to fire an unconditional `xrRequestDisplayRenderingModeDXR(1)` for its
hardcoded Anaglyph default and Windows seeded the same 1 into
`absoluteRenderingModeRequested` — which made the 4-view Quad and 1-view 2D paths
**unreachable from outside the process**: `SIM_DISPLAY_OUTPUT=quad` logged
`Rendering mode changed 4 -> 1` a moment after startup. A pre-v13 runtime names
no active mode; the app then keeps its own default and still issues no request.

**Windows gates adoption on the mode being 3D** (`xr.renderingModeDisplay3D`),
unlike macOS/Linux which adopt verbatim: a real panel commonly reports its 2D
mode active at startup and stays there until something asks for 3D, so adopting
that would open this 3D demo in mono. A 2D active mode therefore falls back to
mode 1, the first 3D mode — and the log line says which branch ran.

**Exercising N-view headlessly (macOS).** `SIM_DISPLAY_OUTPUT` picks the
sim_display mode for BOTH the runtime and the app's pre-session hint, and the
indices must match `sim_display_device.c`: `2d`/`passthrough`=0, `anaglyph`=1,
`sbs`=2, `squeezed`=3, `quad`=4. Keys `0`–`4` select a mode directly, `V`
cycles. Check the runtime's `Atlas blit: view_count=…, eff_tiles=…,
layer_view_count=…` WARN to confirm what actually reached the compositor.

**Linux is the exception:** it renders a FIXED 2-tile horizontal atlas, so it
adopts the active mode's count but `DxrClampSubmitViewCount` cuts a 4-view mode
back to 2 and logs once. It honours a 1-view mode correctly; rendering Quad
there needs the tile layout generalised to the mode's `cols × rows` grid first.

Lint any leg with
`python3 <runtime>/scripts/check_displayxr_app.py <leg-dir>` — it recognises
`DxrSelectViewConfigType` as the opt-in (INV-3.1).

### Renderer conventions (important)
- **Internal target sized to the swapchain** (not per-eye); recreated only on
  swapchain-size change. `renderEye` sets viewport/scissor to the per-eye tile
  and blits `[0,0..vp]` into the swapchain at `(vpX,vpY)`. Mirrors gs_renderer.
- **ZDP-relative clip planes.** `display3d_compute_view/_views` take
  `(near_offset, far_offset)` — **absolute** offsets in virtual-display-height
  (`vH`) units, NOT fractions — and compute per-eye `near = ez − near_offset`,
  `far = ez + far_offset` from each eye's perpendicular distance to the
  convergence plane (`eye_scaled.z`). Callers pass `near_offset = vH`,
  `far_offset = 1000·vH`; **transparent mode passes `far_offset = 0`** (far at
  the ZDP → foreground only). Anchored to `vH` so the band no longer scales with
  `ez` (large scenes don't clip). macOS has no transparent mode → `far_offset =
  1000·vH` there.
- **IBL** is generated once at init from a procedural analytic sky (`sky.glsl`):
  BRDF LUT + irradiance cube + roughness-mipped prefiltered cube; split-sum in
  `pbr.frag`. The **skybox** samples the prefiltered cube at a high mip
  (blurred) — sharp far features cause lightfield cross-talk.
- **sRGB** base-color/emissive are decoded in the shader (textures uploaded
  UNORM). **Normal mapping is tangent-free** (screen-space derivative frame).
- **stb / tinygltf:** `model_loader_gltf.cpp` uses `TINYGLTF_NO_STB_IMAGE` + a
  custom image-loader callback calling `stbi_load_from_memory`; the OBJ/FBX/USD
  backends decode textures via `model_loader_material.cpp` the same way. The stb
  *implementation* comes from `common/d3d11_renderer.cpp` on Windows and
  `common/stb_image_impl_macos.cpp` on macOS — do NOT define
  `STB_IMAGE_IMPLEMENTATION` in any model_common TU (duplicate-symbol clash).
  `model_loader_gltf.cpp` is the one TU that defines `TINYGLTF_IMPLEMENTATION`;
  `model_loader_obj.cpp` likewise owns `TINYOBJLOADER_IMPLEMENTATION`.

### Loader limits (today) → these are the next phases
The avatar bundles an **FBX** (the tiger) and has no in-app load UI, but the
inherited `model_common` loader still accepts every format below — pass a path as
the first CLI arg to float a different character.
- **Multi-format:** glTF/STL/OBJ/FBX/USD all load (see PORTING.md → *Multi-format
  import* for the per-backend table). **FBX skins + animates** (ufbx skins +
  baked anim-stack clips wired into the same ModelData fields the glTF path uses;
  no blend shapes yet, and non-skinned meshes don't follow node animation);
  **USD honours base-color/emissive textures + PBR factors** but not
  normal/metallic-roughness maps yet. Non-glTF material fidelity is best-effort
  (Phong→MR shim for OBJ/FBX).
- **No Draco** mesh compression, **no KTX2/Basis** textures (stb = PNG/JPEG only).
See **PORTING.md** for the phased roadmap.

## Build

### Windows (local dev)
Use **`scripts\build-with-deps.bat`** — it sets vcvars64 + `OpenXR_ROOT` +
Vulkan SDK then runs cmake. The bare `scripts\build_windows.bat` assumes you're
already in a VS dev environment and will fail otherwise. Output:
`build\windows\avatar_handle_vk_win.exe` (+ bundled openxr_loader.dll +
`avatar.fbx`/`rgb.jpg` copied next to it). `model_common` FetchContents
tinygltf + glm. Kill any running instance before rebuilding, or the linker hits
`LNK1104` on the locked exe; verify the build printed `=== DONE ===` + a
`Linking ... avatar_handle_vk_win.exe` line.

**Dev-build dependency rule (don't regress).** The Windows + macOS dev scripts
**auto-provision the OpenXR loader**, pinned to the same spec rev as the vendored
`openxr_includes/` headers (`XR_CURRENT_API_VERSION`, currently 1.1.51) — never
hardcode an SDK path (`C:/dev/openxr_sdk`, `C:/VulkanSDK/<ver>`); Vulkan comes
from the `VULKAN_SDK` env. A fresh clone must build with only VS 2022 + Ninja +
the Vulkan SDK. Keep all three pins equal: CI (`build-windows.yml`) == dev script
== header rev. This is a **dev clone-and-build** concern only — the released
installer always provisioned the loader via CI and bundles `openxr_loader.dll`
next to the exe, so it was never affected.

### macOS (local dev)
`./scripts/build_macos.sh` (builds the OpenXR loader from source, pulls
Vulkan/MoltenVK via brew). Run via **`./scripts/run_macos_dev.sh`**, not the
bare binary (the dev launcher aligns the app + runtime on one Vulkan loader).
`./scripts/build_macos.sh --installer` builds the `.pkg`.

### Linux (local dev — build-green only, #21)
`./scripts/build_linux.sh` (builds the OpenXR loader from source pinned to
1.1.43, uses system `libvulkan-dev`). Produces `build/linux/avatar_handle_vk_linux`
and runs via **`./scripts/run_avatar_linux.sh`**. The Linux entry point is
`linux/main.cpp` — a minimal Vulkan + OpenXR frame loop driving the same
cross-platform `model_common/ModelRenderer` the macOS/Windows peers use.
Windowing is **hosted-NULL** (the runtime self-creates its window); the faithful
app-owned X11 window via `XR_DXR_xlib_window_binding` (header already vendored in
`openxr_includes/`) is **Phase-3** work, gated on the Linux runtime + a GPU + an
X server. This is **build-green only** — CI compiles it on `ubuntu-latest`
(`build-linux.yml`, dispatch/`linux*`-branch only, not required); on-screen
validation is deferred. Recipe: `docs/guides/linux-demo-port.md` in
`displayxr-runtime`.

### CI (`.github/workflows/`)
`build-windows.yml` + `build-macos.yml` run on **`pull_request` + push to main**
(build-validation — they compile the app + installer/.pkg, nothing publishes)
and on **`v*` tags** (release: build + attach both installers to the GH Release +
dispatch `versions-bump` to displayxr-runtime). So every PR is build-checked on
both platforms — keep it that way.

## Self-verifying a render (no 3D display needed)
Two capture paths, both readable on a flat monitor:
- **Atlas capture (`I`)** — dumps the multi-view atlas to
  `%USERPROFILE%\Pictures\DisplayXR\<name>_atlas_<vc>_<cols>x<rows>.png` (skipped
  for 1×1 mono), via runtime-owned `xrCaptureAtlasDXR`. Good for
  geometry/shading/framing + the bottom-70% confinement.
- **Silhouette dump (`DXR_DUMP_SILHOUETTE=1`)** — writes the click-through mask
  **as published** to `%TEMP%\avatar_silhouette.png` (~1/sec; white = covered by
  the raw alpha test, grey = added by the dilation, black = clipped away). Diff
  it against the rendered frame to see exactly what the shaped window is
  deleting — that comparison is the point, and dumping raw alpha (as this used
  to) could not show it.

The compositor file-trigger screenshot does **not** work for the in-process VK
compositor — use the two dumps above, or ask the user to eyeball the live SR
display (the harness can't reach the window for input or see the 3D output).
Window title is `DisplayXR 3D Avatar`. Caveat: an atlas capture is a single
arbitrary frame — for the animated tiger the pose is uncontrolled; recapture.

## Shell tile
`windows/displayxr/avatar_handle_vk_win.displayxr.json` (schema_version 1, `id`
`avatar`) carries the sidecar + **per-app-named** icons `avatar_icon.png` (2D) +
`avatar_icon_sbs.png` (3D, sbs-lr). The names MUST be unique: the shell's
`%ProgramData%\DisplayXR\apps\` dir is shared by all demos and icon paths resolve
relative to it — a generic `icon.png` collides. Regenerate real icons from a live
atlas capture with **`/make-app-logos`** (run from the runtime repo); it splits
the centre two views into the 2D + 3D logos and updates the manifest. Lint the
app against the authoring rules with
`python3 scripts/check_displayxr_app.py windows/` (from the runtime repo).

## Releasing
Wired as a `/dxr-release` component — run **`/dxr-release avatar vX.Y.Z`** from
the `displayxr-runtime` hub (not from here; the skill clones this repo itself).
It pushes an empty `Release vX.Y.Z` marker to `main`, tags it, and watches CI,
which builds the installers, attaches them to the GH Release, and dispatches
`versions-bump` to displayxr-runtime — bumping the `avatar_demo` field and
mirroring `versions.json` to `displayxr-installer`. The skill then signs the
Windows installer on the signing runner and re-uploads it over the CI asset; the
macOS `.pkg` stays unsigned (separate Apple track). Independent cadence from the
runtime. Verified end to end on v0.10.2.

## Coding conventions
- C++17/20, Vulkan 1.0+, Objective-C++ on macOS.
- `lower_snake_case` files/functions, `PascalCase` C++ types.
- **Multiview-first language**: `tile` / `view` / `atlas`. NEVER `stereo`,
  `left+right eye`, or `SBS` in code/comments/docs/chat (the SBS *logo layout*
  string `sbs-lr` is the one allowed exception — it's the manifest schema value).
- CMake breaks on spaces in dev paths; quote paths, use relative manifest paths.

## Sibling repos
| Repo | Purpose |
|---|---|
| [`displayxr-runtime`](https://github.com/DisplayXR/displayxr-runtime) | The runtime (+ versions.json hub, release skills). |
| [`displayxr-demo-modelviewer`](https://github.com/DisplayXR/displayxr-demo-modelviewer) | **Parent demo** — this app's `model_common/` renderer comes from there. |
| [`displayxr-demo-gaussiansplat`](https://github.com/DisplayXR/displayxr-demo-gaussiansplat) | Sibling splat-viewer demo; shares the `common/` view math. |
| [`displayxr-installer`](https://github.com/DisplayXR/displayxr-installer) | Windows meta-installer bundle (chains the demos). |
| [`displayxr-shell-releases`](https://github.com/DisplayXR/displayxr-shell-releases) | Shell installer (optional add-on). |

## MCP atlas capture (agent-side debugging)

`.mcp.json` registers the `displayxr` MCP server — the DisplayXR MCP adapter
installed by `DisplayXRMCPSetup` (`HKLM\Software\DisplayXR\Capabilities\MCP`).
When that capability is installed, **every OpenXR app process hosts an
in-process MCP server**, so a running `avatar_handle_vk_win` exposes:

- `capture_frame` — writes the composed atlas as
  `%TEMP%\displayxr-mcp-capture-<pid>-<frame>.png` and returns the path
  (modes: `post-compose` default, `projection-only`). Read the PNG to see
  exactly what the display processor receives, per tile.
- `diff_projection`, `get_kooima_params`, `get_submitted_projection`,
  `get_display_info`, `get_runtime_metrics`, `tail_log`.

Workflow:

1. **Launch the app first**, then start the Claude session — or run `/mcp` →
   reconnect `displayxr` after launching (the adapter binds at spawn time).
2. `--target auto` attaches shell → service → unique app PID. If more than
   one OpenXR app is running, pin it: change args to `--target pid:<pid>`.
3. Call `capture_frame`, then Read the returned PNG path.

Non-Windows: set `DISPLAYXR_MCP_ADAPTER` to the adapter's install path before
launching Claude (the `.mcp.json` default is the Windows path).
