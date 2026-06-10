# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working on this repo.

## Project Overview

DisplayXR Demo — **3D Model Viewer**. Real-time PBR model viewer for
glasses-free 3D displays, on the DisplayXR OpenXR runtime via Vulkan (Windows)
and MoltenVK (macOS). Loads glTF 2.0 (`.glb`/`.gltf`), STL, OBJ, FBX, and USD
(`.usdz`/`.usd`/`.usda`/`.usdc`), renders with asymmetric per-eye
Kooima projection. Standalone repo, independent release cadence; `common/` +
`openxr_includes/` were seeded from the runtime and are maintained here.

**Status: shipped & working on Windows + macOS.** The renderer is fully
implemented (this is NOT a skeleton — ignore any old "porting baseline"
phrasing). Released: v0.1.0 (initial), v0.2.0 (macOS + ZDP clip). Bundled
`sample.glb` = Khronos DamagedHelmet (CC BY 4.0), auto-loaded at startup.

## Runtime dependency

Requires **DisplayXR runtime ≥ v1.9.1** (v1.9.1 has the per-app VK-native
compositor in-place DComp resize fix this demo relies on for clean window
resizing). Install `DisplayXRSetup-*.exe` from the
[runtime releases](https://github.com/DisplayXR/displayxr-runtime/releases).
Apps load the runtime via the registry-resolved manifest (no `XR_RUNTIME_JSON`).

## Architecture

```
windows/main.cpp, macos/main.mm  — platform entry: window, OpenXR session,
                                    input/HUD, per-frame view/projection, atlas
                                    capture ('I'), file load (L / drag-drop)
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
                                    camera3d_view (unused), input, HUD, stb
openxr_includes/                  — vendored OpenXR + DisplayXR ext headers
```

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
- **Multi-format:** glTF/STL/OBJ/FBX/USD all load (see PORTING.md → *Multi-format
  import* for the per-backend table). **FBX now skins + animates** (ufbx skins +
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
`build\windows\model_viewer_handle_vk_win.exe` (+ bundled openxr_loader.dll +
sample.glb copied next to it). `model_common` FetchContents tinygltf + glm.

### macOS (local dev)
`./scripts/build_macos.sh` (builds the OpenXR loader from source, pulls
Vulkan/MoltenVK via brew). Run via **`./scripts/run_macos_dev.sh`**, not the
bare binary (the dev launcher aligns the app + runtime on one Vulkan loader).
`./scripts/build_macos.sh --installer` builds the `.pkg`.

### CI (`.github/workflows/`)
`build-windows.yml` + `build-macos.yml` run on **`pull_request` + push to main**
(build-validation — they compile the app + installer/.pkg, nothing publishes)
and on **`v*` tags** (release: build + attach both installers to the GH Release +
dispatch `versions-bump` to displayxr-runtime). So every PR is build-checked on
both platforms — keep it that way.

## Self-verifying a render (no 3D display needed)
Pass a model path as the first CLI arg to skip auto-load:
`model_viewer_handle_vk_win.exe <path.glb>`. Then press **`I`** to dump the
multi-view atlas to `%USERPROFILE%\Pictures\DisplayXR\<model>-<cols>_<rows>x<n>.png`
(skipped for 1×1 mono layouts) — readable to eyeball geometry/shading/framing
without a glasses-free display. To drive it headlessly, launch the exe then
`AppActivate` its window (title `DisplayXR 3D Model Viewer`) + SendKeys `i` via
`WScript.Shell`, and read the newest PNG in that dir. Caveat: the capture is a
single arbitrary frame — for animated models the timing is uncontrolled, so a
"wrong" pose/face-on grab means bad luck, not a bug; recapture.

## Shell tile
`windows/displayxr/` + `macos/displayxr/` carry the `.displayxr.json` sidecar +
**per-app-named** icons `model_viewer_icon.png` (2D) + `model_viewer_icon_sbs.png`
(3D, sbs-lr). The names MUST be unique: the shell's `%ProgramData%\DisplayXR\apps\`
dir is shared by all demos and icon paths resolve relative to it — generic
`icon.png` collides (it clobbered the gaussiansplat demo once). The icons are a
square-cropped render of the bundled model. `scripts\dev_register.bat` points
the shell launcher at your dev build.

## Releasing
Tag `vX.Y.Z` → CI builds both installers, attaches them to the GH Release, and
dispatches `versions-bump` (`modelviewer_demo` field) to displayxr-runtime,
which mirrors `versions.json` to `displayxr-installer`. The Windows meta-installer
bundle ships this demo (`/installer-release` on the bundle, when ready).
Independent cadence from the runtime.

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
| [`displayxr-demo-gaussiansplat`](https://github.com/DisplayXR/displayxr-demo-gaussiansplat) | Sibling splat-viewer demo; shares the `common/` view math. |
| [`displayxr-installer`](https://github.com/DisplayXR/displayxr-installer) | Windows meta-installer bundle (chains this demo). |
| [`displayxr-shell-releases`](https://github.com/DisplayXR/displayxr-shell-releases) | Shell installer (optional add-on). |

## MCP atlas capture (agent-side debugging)

`.mcp.json` registers the `displayxr` MCP server — the DisplayXR MCP adapter
installed by `DisplayXRMCPSetup` (`HKLM\Software\DisplayXR\Capabilities\MCP`).
When that capability is installed, **every OpenXR app process hosts an
in-process MCP server**, so a running `model_viewer_handle_vk_win` exposes:

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
