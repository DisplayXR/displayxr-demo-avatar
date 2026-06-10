# Roadmap

The initial port — turning the gaussiansplat skeleton into a working glTF 2.0
PBR viewer on Windows + macOS — is **complete and shipped** (v0.1.0 Windows,
v0.2.0 macOS + ZDP clip). This file is now the forward roadmap. For how the
renderer works today, see `CLAUDE.md` → *Renderer conventions*.

## Done (the port)

- **`model_common/`** — real renderer, not stubs. tinygltf load (factors +
  textures, world-baked node transforms, AABB), interleaved vertex/index
  upload, metallic-roughness Cook-Torrance GGX, split-sum IBL (BRDF LUT +
  irradiance + prefiltered-env from a procedural analytic sky), blurred skybox,
  sRGB-correct + tangent-free normal mapping, internal image → per-eye viewport
  blit. View math mirrors gs_renderer's Y-flip.
- **Platform glue** — `windows/main.cpp` + `xr_session.cpp` and `macos/main.mm`
  retargeted to `ModelRenderer` (`loadModel`/`hasModel`/`getRobustSceneBounds`,
  `.glb/.gltf` dialogs + drag-drop, `sample.glb` auto-load, 1.2× auto-fit).
- **ZDP clip planes** — `display3d_compute_view/_views` take
  `(near_offset, far_offset)` as absolute `vH`-unit offsets; per-eye
  `near = eye.z − near_offset`, `far = eye.z + far_offset`. Callers pass
  `near_offset = vH`, `far_offset = 1000·vH`; transparent → far_offset 0.
- **Bundled `sample.glb`** = Khronos DamagedHelmet, copied next to the exe.
- **Installer + sidecar** — Windows NSIS + macOS `.pkg`; per-app icon names
  (`model_viewer_icon{,_sbs}.png`). Wired into the meta-installer bundle.
- **CI** — `build-windows.yml` + `build-macos.yml` validate every PR + main
  push on both platforms; tags also build+attach installers and dispatch the
  `versions-bump`.

## Next: animation + skinning (the big one)

glTF carries skeletal animation, node (TRS) animation, and morph targets; the
viewer currently renders the **bind pose** only. Phased plan:

1. **Node/TRS animation (no skin)** — parse `animations[]` channels
   (translation/rotation/scale) + samplers (input/output accessors, LINEAR /
   STEP / CUBICSPLINE). Per frame, sample the active animation → per-node local
   TRS → re-walk the node hierarchy to world matrices. Today `model_loader`
   bakes world transforms **once at load**; this must become a per-frame pass
   (keep the static fast-path when no animation is active). Cheapest first win;
   no shader change (push-constant `model` matrix already per-primitive).
2. **Skinning** — extend `ModelVertex` with `joints0` (u16x4) + `weights0`
   (f32x4) vertex attributes; parse `skins[]` (joints list + inverseBindMatrices
   accessor). Add a **joint-matrix SSBO/UBO** (set 3) and do linear-blend
   skinning in `pbr.vert`. Joint matrices = `globalNode * inverseBind`,
   recomputed per frame from the step-1 node walk. Watch the UBO size cap →
   prefer an SSBO for large skeletons.
3. **Morph targets** — accessor deltas + per-node weights; either CPU-blend into
   a dynamic vertex buffer or pass targets as extra attributes/SSBO. Lower
   priority (fewer sample assets use it).
4. **Playback UI** — HUD line (clip name, time, play/pause), a key to cycle
   `animations[]`, auto-play the first clip on load. Keep it minimal.

Suggested bundled animated sample: Khronos **CesiumMan** or **Fox** (small,
CC-licensed, exercises skin + clips).

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
The four open-dialog filters (Windows spatial picker + Win32 fallback, macOS
`NSOpenPanel`) and `model_validate_file` gate the same extension set.

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
