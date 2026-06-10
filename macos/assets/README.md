# Bundled demo model

`sample.glb` is the model the app auto-loads at startup. The build scripts copy
it next to the executable and the installer stages it (the same pattern the
Gaussian-splat demo uses for `butterfly.spz`).

## Current asset — attribution

`sample.glb` is **"Battle Damaged Sci-fi Helmet — PBR"** (a.k.a. *DamagedHelmet*)
from the [Khronos glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/DamagedHelmet)
collection.

- Author: **theblueturtle_** (helmet), glTF conversion by ctxwing.
- License: **CC BY 4.0** — <https://creativecommons.org/licenses/by/4.0/>
- Attribution **must** be retained when redistributing this asset (installer
  about/credits + release notes).

To swap the default model, replace `sample.glb` and update this attribution.
CC0 alternatives (no attribution needed) live in the same Khronos repo, e.g.
`Box`, `BoxTextured`, `Avocado`.
