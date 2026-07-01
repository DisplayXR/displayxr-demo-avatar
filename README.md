# DisplayXR Demo — 3D Avatar

A transparent, click-through **3D avatar** that floats over your desktop on a
glasses-free 3D display, built on the DisplayXR runtime via OpenXR with Vulkan.
The bundled character — **Leo**, an animated tiger — is weaved in true multiview
3D, faces the viewer (head-tracked billboard), and sits behind a flat 2D speech
bubble. Clicks outside the character's silhouette fall straight through to
whatever desktop window is behind it, so Leo is an ambient companion, not a
window that gets in your way.

It is a native-Vulkan **`_handle` OpenXR app** derived from the
[`displayxr-demo-modelviewer`](https://github.com/DisplayXR/displayxr-demo-modelviewer)
renderer (`model_common/`), specialised into an avatar shell.

> **Requires the DisplayXR runtime v1.9.1 or newer** (Windows). Download
> `DisplayXRSetup-*.exe` from the
> [`displayxr-runtime` releases page](https://github.com/DisplayXR/displayxr-runtime/releases).
> v1.9.1+ ships the Vulkan transparent-window bridge + in-place resize this demo
> relies on; older runtimes produce a broken/black window or flicker on resize.
> The speech bubble additionally needs a runtime that advertises
> `XR_EXT_local_3d_zone` (Local2D); without it the avatar still renders, just
> with no bubble.

## What makes it an "avatar" (not a model viewer)

- **Transparent by default** — the window is borderless (`WS_POPUP`) and
  always-topmost; the runtime composes the weaved character *under* the desktop,
  so only the tiger and its bubble are visible. `Ctrl+T` toggles the opaque path
  for debugging.
- **Per-pixel click-through** — a throttled silhouette pass reads back the
  character's alpha each tick and shapes the window with `SetWindowRgn`, so the
  OS routes input *outside* the silhouette to the desktop window behind
  (natively, cross-process). The Unity-plugin recipe, in a native app.
- **Face-the-viewer billboard** — the avatar's yaw tracks the tracked head
  centre (from eye tracking), smoothed and gated on tracking lock to avoid
  warmup jitter.
- **Depth dolly with display-plane clip** — `W`/`S` push the avatar toward / away
  from the viewer *through* the display plane; in transparent mode anything
  behind the plane is clipped, so only the popping-out part shows.
- **2D speech bubble over 3D** — one `XrCompositionLayerLocal2DEXT` over the top
  ~30 % band (implicit M=0 mask) gives a crisp flat nameplate while the bottom
  70 % keeps weaving. The avatar is confined to the bottom-70 % sub-rect (a
  handle-app Kooima sub-canvas, the equivalent of a texture app's
  `xrSetSharedTextureOutputRectEXT`).

## Controls

| Input | Action |
|---|---|
| `W` / `S` | Dolly the avatar nearer / further in depth (clips at the display plane) |
| Left-click drag | Rotate the avatar (overrides the billboard while dragging) |
| Double-click | Focus / re-pose toward the picked surface point |
| Scroll | Zoom (virtual-display height) |
| `Space` | Reset to the auto-fit pose |
| `V` (or `0`–`8`) | Cycle / select the rendering modes the display runtime advertises |
| `N` / `K` | Next animation clip / play-pause |
| `Ctrl+T` | Toggle transparent background (desktop see-through) |
| `B` | Toggle window decoration — borderless ⇄ title bar for OS move/resize |
| `I` | Capture the multi-view atlas to `%USERPROFILE%\Pictures\DisplayXR\` |
| `F11` | Fullscreen |
| `Esc` | Quit |

There is **no in-app model-load UI** — the avatar auto-loads its bundled tiger.
Pass a model path as the first CLI argument to float a different character
instead (it falls back to the bundled tiger if the path is missing/invalid).

## Build from source (Windows)

### Prerequisites
- CMake ≥ 3.21 + Ninja
- [Vulkan SDK](https://www.lunarg.com/vulkan-sdk/) (includes `glslangValidator`)
- [OpenXR loader](https://github.com/KhronosGroup/OpenXR-SDK) (find_package-visible)
- A DisplayXR-compatible runtime (install via `DisplayXRSetup-*.exe` from
  [displayxr-runtime releases](https://github.com/DisplayXR/displayxr-runtime/releases))

```bat
REM Sets vcvars64 + OpenXR_ROOT + Vulkan SDK, then configures + builds.
scripts\build-with-deps.bat
REM Run (loads the installed runtime via the registry; no XR_RUNTIME_JSON)
build\windows\avatar_handle_vk_win.exe
```
> Use `build-with-deps.bat`, not the bare `build_windows.bat` — the latter
> assumes you are already inside a VS developer environment.

`model_common/` fetches **tinygltf**, **glm**, and **tinyusdz** via CMake
`FetchContent` on first configure (no submodules); **tinyobjloader** and **ufbx**
(the FBX loader the bundled tiger uses) are vendored under
`model_common/third_party/`. The first configure builds tinyusdz from source, so
it is slower.

> **macOS:** a Cocoa/MoltenVK port of the avatar shell is in progress under
> `macos/` but is not yet the supported target — the renderer (`model_common/`)
> is already cross-platform. Track it in [`PORTING.md`](PORTING.md).

## Repo layout

```
.
├── windows/                Platform entry + window handling (Win32 / Vulkan)
├── macos/                  Cocoa / MoltenVK port (in progress)
├── model_common/           Multi-format PBR renderer: loaders + renderer + shaders
├── common/                 Shared helpers: Kooima math, input, HUD/text renderer
├── openxr_includes/        Vendored OpenXR headers (incl. DisplayXR extensions)
├── assets/                 Bundled tiger (FBX + texture) + extra test models
├── installer/              Windows NSIS installer
├── scripts/                Build scripts
└── PORTING.md              Roadmap
```

`common/` and `openxr_includes/` are shared with the other DisplayXR demos and
were seeded from the runtime source tree. The renderer draws on techniques from
the MIT-licensed
[SaschaWillems/Vulkan-glTF-PBR](https://github.com/SaschaWillems/Vulkan-glTF-PBR).

## License

Apache-2.0 — see `LICENSE`. The bundled avatar and any test models carry their own
licenses. (Vendored OpenXR extension headers under `openxr_includes/` remain
BSL-1.0 — see their SPDX headers.)
