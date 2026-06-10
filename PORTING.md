# Roadmap

This demo is the **3D Avatar** — `displayxr-demo-modelviewer`'s renderer
(`model_common/`) wrapped in an avatar shell (transparent click-through floating
character + speech bubble). The Windows shell is **working**; the forward
roadmap is below. For how the renderer works today, see `CLAUDE.md` →
*Renderer conventions*; for the avatar-shell internals, see *Avatar shell* there.

## Done (the avatar shell, Windows)

- **Borderless transparent window** — `WS_POPUP` + topmost + no-redirection
  bitmap; always-on session transparency; `B` toggles decoration for OS
  move/resize; `Ctrl+T` debug-toggles the opaque path.
- **Per-pixel click-through** — throttled silhouette pass (mono eye → alpha
  readback) drives `SetWindowRgn`; input outside the silhouette falls through
  cross-process. `DXR_DUMP_SILHOUETTE=1` dumps the mask.
- **Face-the-viewer billboard** — yaw tracks the tracked head centre, time-based
  smoothing, gated on `isEyeTracking`. Pitch coded but disabled.
- **W/S depth dolly + display-plane clip** — X/Y anchored to the animated
  skeleton centroid; `W`/`S` drive Z through the ZDP clip (transparent →
  `far_offset = 0`, foreground only).
- **Speech bubble (Local2D)** — bottom-70 % Kooima sub-canvas + bottom-70 %
  sub-viewport for the weaved avatar; one `XrCompositionLayerLocal2DEXT` pill
  (auto-fit, balanced, aspect-preserving) + a full-band transparent backer over
  the top 30 %, submitted in a hand-built `xrEndFrame`. Gated on
  `XR_EXT_local_3d_zone`.
- **Bundled avatar** = `assets/tiger/avatar.fbx` (+ `rgb.jpg`), copied next to the
  exe and auto-loaded. Inherited renderer: metallic-roughness GGX + split-sum IBL
  + ufbx **skinning + animation** (the tiger plays its first clip).
- **Inherited renderer + ZDP clip + multi-format loader** — see the table below;
  these came from modelviewer unchanged.

## Next (avatar)

1. **App logos** — regenerate `windows/displayxr/avatar_icon{,_sbs}.png` from a
   live atlas capture (`/make-app-logos`); they are still modelviewer
   placeholders. Lint with `scripts/check_displayxr_app.py`.
2. **macOS port** — `macos/main.mm` is a partial avatar-shell port. macOS has no
   transparent-window bridge or `SetWindowRgn` analogue yet, so the click-through
   + compose-under-desktop story needs a Cocoa/Metal-layer design; the renderer
   is already cross-platform.
3. **`/dxr-release` wiring** — add an `avatar_demo` field to the runtime's
   `versions.json` + a `versions-bump` dispatch in CI so the bundle can ship it.
4. **Polish** — optional: fill the full top-30 % band with a balanced centered
   bubble (currently the pill hugs the text); re-enable the pitch billboard
   (`FACE_PITCH_SIGN`); a second/idle greeting line.

## Multi-format import (done)

The loader is a thin **format dispatcher** (`model_loader.cpp`) routing by
extension to a per-format backend, each filling the same `ModelData` the renderer
consumes (adding a format is front-end work only):

| Format | Backend | Parser | Materials |
|---|---|---|---|
| `.glb`/`.gltf` | `model_loader_gltf.cpp` | tinygltf (FetchContent) | PBR-native |
| `.stl` | `model_loader_stl.cpp` | hand-rolled, no dep | single neutral default |
| `.obj` | `model_loader_obj.cpp` | tinyobjloader (vendored) | Phong `.mtl` → MR shim |
| `.fbx` | `model_loader_fbx.cpp` | ufbx (vendored) | PBR maps, Phong fallback |
| `.usd*` | `model_loader_usd.cpp` | tinyusdz/tydra (FetchContent) | UsdPreviewSurface (PBR) |

OBJ + FBX share `model_loader_material.{h,cpp}` (texture decode + Phong→roughness).
The avatar has no in-app load UI (it auto-loads the bundled FBX), but
`model_validate_file` still gates the CLI model-path arg against this extension
set, so any of these can be floated as the avatar via the command line.

**FBX skinning / animation (done):** `model_load_fbx` now emits skins (per-vertex
top-4 joints/weights; `inverseBind` = ufbx `cluster->geometry_to_bone`; skinned
meshes kept in geometry space), the node hierarchy (`HELPER_NODES` inherit mode
so a plain parent×local walk matches ufbx's `node_to_world`), and each anim-stack
baked to 30 fps Linear keyframes via `ufbx_evaluate_transform` (constant channels
dropped) — all into the same `ModelData` fields the glTF path fills, so the
renderer skins + animates with no renderer/shader changes. The skeleton-centroid
display anchor gets a visual-centre correction (computed in
`recomputeAnimatedBounds`) so joint-free geometry like a hat doesn't ride
off-centre. Remaining FBX gaps: **blend shapes** (morph targets) aren't read, and
**non-skinned meshes don't follow node animation** (still world-baked).

**Format follow-ups:**
- **USD textures beyond base-color/emissive** — UsdPreviewSurface keeps
  metallic + roughness as separate single-channel maps and normals as a normal
  map; today USD honours those as factors + base/emissive textures only.
- **OBJ/FBX texture dedup** — repeated `map_Kd`/embedded textures are decoded
  per material reference (minor; no caching yet).

## Other follow-ups (smaller)

- **`pickSurface`** — ray/triangle intersection vs the loaded mesh (currently a
  no-op, so double-click focus does nothing). Useful for shell focus + future
  inspect features.
- **Draco** mesh decompression (`KHR_draco_mesh_compression`) — many real-world
  `.glb`s use it; needs the Draco decoder wired into `model_loader`.
- **KTX2 / Basis Universal** textures (`KHR_texture_basisu`) — stb is
  PNG/JPEG-only today; GPU-compressed textures need libktx + a transcoder.
- **`KHR_materials_*`** extensions (clearcoat, transmission, emissive strength)
  — incremental PBR fidelity once the above land.
