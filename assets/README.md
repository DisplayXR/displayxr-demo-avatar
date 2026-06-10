# Test assets

Animation/skinning test models, loaded via the CLI arg or drag-drop — **not**
bundled next to the executable (the shipped default is `windows/assets/sample.glb`
= DamagedHelmet). Run e.g.:

```
build\windows\model_viewer_handle_vk_win.exe assets\CesiumMan.glb
```

| File | Exercises | Source / license |
|---|---|---|
| `BoxAnimated.glb` | Node/TRS animation (Phase 1) | Khronos [glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets), CC0 1.0 |
| `CesiumMan.glb` | Skinning + a walk clip (Phase 2) | Khronos [glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) — © Cesium, CC BY 4.0 |
| `AnimatedMorphCube.glb` | Morph targets + animated weights (Phase 3) | Khronos [glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) — © 2017, CC0 1.0 |
| `Fox.glb` | Multi-clip skinning — Survey/Walk/Run (Phase 4 clip cycling) | Khronos [glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) — © PixelMannen, CC0 1.0 |
