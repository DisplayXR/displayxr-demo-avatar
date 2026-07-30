# iGPU perf work — findings, knobs, and what's left

Context: Lenovo benchmarked the Unity desktop-avatar sample at ~18 % GPU and this
native demo at ~33 % on their iGPU, same content and window size. Goal was
single-digit iGPU usage with no visible quality regression.

Measured on a Meteor Lake box (Intel Arc iGPU driving a Leia DS1_156, 3840×2160
panel), tile 811×1066, clip paused unless noted.

## The finding that reframes the task

The renderer is **not** where the app's GPU time goes.

| | GPU |
|---|---|
| model render, all views + silhouette | **3.9 %** |
| avatar process total | **17.8 %** |
| dwm | ~16–22 % |

This is a `_handle` app, so the in-process compositor **and the Leia weave** are
attributed to the app process — no `displayxr-service` is running. So ~14 of the
app's 18 points are compositor + weave + copy-engine blits, all driven by
**submission rate**, not by how cheap the render is. That caps every
render-shaving item at a few points and makes throttling the only lever with the
range to reach single digits.

dwm at ~16–22 % is the largest single consumer and is stable across every
renderer config. It was on the original "don't chase" list because both samples
use transparency — but both using it doesn't mean both cost the same.

## Results

| Item | Change | Result |
|---|---|---|
| 1 | Render targets sized to the viewport, not the zone swapchain | **1708 MB → 20 MB** |
| 2 | MSAA 8× → 2× | 0.45 → 0.26 ms/view; quality signed off by eye |
| 3 | Per-view `vkQueueWaitIdle` → UBO ring + per-frame fence | **+40 % throughput** (85 → 119 views/s) |
| 4 | Silhouette 640×360 → 256×144, scoped fence | small; see below |
| 5 | Feather pass scissored to a 4-strip border band | byte-identical, no measured cost |
| 7 | Idle throttling gated on `isTracking` | **−25 % app GPU** (paired, 3/3) |
| 8 | `DXR_AVATAR_SKIN_HZ` cap | CPU/upload only, default off |

Item 7, interleaved A/B, 3 reps, viewer present throughout (`tracked% = 99–100`):

```
rep  throttle  tile views/s  app GPU %
 1   off       118.7         14.48
 1   on         66.5         10.30     -28.9 %
 2   off       118.7         14.86
 2   on         83.9         10.81     -27.3 %
 3   off       119.1         19.23
 3   on        106.8         15.31     -20.4 %
```

Separately, with **no viewer tracked**, one run measured 13.6 % → 7.7 % — single
digits. That is a single observation and has not been repeated.

## Knobs

All default to prior behaviour except where noted, so a baseline and a fix are
reachable from one binary.

| Env | Default | Effect |
|---|---|---|
| `DXR_AVATAR_MSAA` | 2 | 1 / 2 / 4 / 8. `=8` restores the old default |
| `DXR_AVATAR_TARGET_SIZING` | viewport | `=swapchain` restores the old sizing |
| `DXR_AVATAR_IDLE_THROTTLE` | 1 | `=0` disables idle throttling |
| `DXR_AVATAR_IDLE_HZ` | 15 | frame rate while idle, viewer present |
| `DXR_AVATAR_AWAY_HZ` | 5 | frame rate while no viewer tracked |
| `DXR_AVATAR_IDLE_GRACE_MS` | 2000 | quiet time before throttling engages |
| `DXR_AVATAR_SKIN_HZ` | 0 (off) | cap skinning rate; visible judder if set |
| `DXR_AVATAR_GPUTIME` | off | timestamp queries, bucketed by viewport, + views/s |
| `DXR_AVATAR_PERF_LOG` | `%TEMP%\dxr_avatar_perf.log` | perf sink (WinMain app: printf is not visible) |
| `DXR_AVATAR_IDLE_DEBUG` | off | counts what holds the app awake |
| `DXR_AVATAR_START_PAUSED` | off | start with the clip paused (reach the idle path headlessly) |
| `DXR_AVATAR_CAPTURE_AFTER_MS` | off | fire one atlas capture N ms in |
| `DXR_AVATAR_DUMP_VIEWMATS` | off | sample per-view matrices periodically |

The last four are test hooks. They earned their keep and are all inert unless
set, but they are test surface in a shipping demo and want a keep/strip call.

## Measurement traps — all of these cost time here

- **Nothing is at steady state for the first ~4.5 s.** Eye tracking has not
  engaged, so both eyes sit on the SR nominal viewer and render *identically*. A
  one-shot capture or matrix dump inside that window shows collapsed stereo that
  is not real. This produced a wrong "the panel isn't showing 3D" conclusion.
- **Do not eyeball atlas halves.** Two byte-identical views read to the eye as
  two different head angles. Diff them numerically (mean abs RGB; ~20 with
  disparity, 0.00 without).
- **GPU % on this box is ±3 points run to run**, which is wider than most of
  these effects. `views/s` from `DXR_AVATAR_GPUTIME` is stable to ±0.2 % and is
  the metric to lead with. Always pair A/B **within** a rep; comparing across
  reps produced a bogus −44 % where the honest paired figure is −25 %.
- **Log `tracked%` during any throttle measurement.** Whether a face is in front
  of the panel changes the frame rate independently of the throttle flag.
- **MSAA A/B cannot be pixel-compared** while head tracking moves the camera
  between runs. Compare edge character, not pixels.
- `DXR_AVATAR_GPUTIME` adds a per-view fence wait, so absolute frame rates from a
  GPUTIME run are slightly pessimistic. Fine for comparing configs.

Harness: `ab_rigorous.ps1` (interleaved A/B) and `gpu_attrib.ps1` (per-process
GPU-engine attribution) in the session scratchpad.

## Landmines in what shipped

- **The per-view `vkQueueWaitIdle` was load-bearing.** One shared
  `uniformBuffer_` was rewritten per view, so the drain was the only thing
  stopping view 2 clobbering view 1's matrices. Removing it without ringing the
  UBO silently collapses stereo — frame rate up, GPU down, 3D quietly broken.
  Now a `UNIFORM_BUFFER_DYNAMIC` ring with one block per view.
- **`beginFrame()` must precede `updateAnimation()`.** It waits the previous
  frame's submissions; `updateAnimation` rewrites the joint SSBO, which is
  per-frame rather than per-slot and is what bounds us to one frame in flight.
  Ringing the joint buffer is the prerequisite for 2–3 frames in flight.
- **The feather band must be four non-overlapping strips.** An overlapping ring
  multiplies the corners twice and darkens them.
- **Removing stalls raises frame rate, which raises GPU %.** Item 3 alone would
  have read as a regression against a GPU-% target. It has to land with
  throttling or a cap.

## Not done

- **Item 6 — render directly into the runtime swapchain image.** Needs
  per-swapchain-image framebuffers and the MSAA resolve retargeted at the
  swapchain. Ceiling is the copy engine, measured at 1.2–2.8 %.
- **Item 9 — `xrGetDisplayZoneRecommendedViewSizeDXR` scale factor**
  (`oxr_display_zones.c`, `displayxr-runtime`). Genuinely trades quality. Note
  the measurement narrows its case: the weave runs at panel resolution
  regardless of tile size, so its reach is smaller than "~4× the fragments"
  suggests.
- **Item 4's depth/alpha-only silhouette pipeline.** Measured silhouette cost is
  ~0.3 % of GPU, which does not justify a second pipeline + shader + render pass.

## Open question for whoever picks this up

"Single-digit iGPU" is met with no viewer present. With a viewer present the app
sits at ~15 %, and getting below that means the weave (~9 %) and dwm (~16–22 %),
neither of which is in this repo. Worth settling which reading the target refers
to — the avatar process's Task Manager column, or the system total. On this box
those are ~15 % and ~32 %.
