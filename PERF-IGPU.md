# iGPU perf work — findings, knobs, and what's left

Context: Lenovo benchmarked the Unity desktop-avatar sample at ~18 % GPU and this
native demo at ~33 % on their iGPU, same content and window size. Goal was
single-digit iGPU usage with no visible quality regression.

Measured on a Meteor Lake box (Intel Arc iGPU driving a Leia DS1_156, 3840×2160
panel), window 463×812, tile 811×1066 per view, 60 Hz → ~117 views/s.

## Read the measurement section first

Every number below was re-measured on 2026-07-31 with the counter method in
[Measurement](#measurement). The earlier figures came from spot-sampling
`\GPU Engine(*)\Utilization Percentage`, which turned out to average only ~7
samples per run — wide enough that one recorded result (**item 9 at −15.7 %**) did
not survive re-measurement. Corrected values are marked.

## The finding that reframes the task

The renderer is **not** where the app's GPU time goes, and most of what remains
does not scale with frame rate either.

This is a `_handle` app, so the in-process compositor **and the Leia weave** are
attributed to the app process — no `displayxr-service` is running. Throttling the
app from ~117 views/s to ~10 views/s (12× fewer submissions) only takes the app
from ~15.2 % to ~9.8 %:

| app-process GPU | ~117 views/s | ~10 views/s |
|---|---|---|
| 3d engine | 11.6–12.0 | 8.2–8.6 |
| copy engine | 3.4–3.6 | 1.4–1.6 |
| **total** | **~15.2** | **~9.8** |
| of which the model render itself | 1.5–2.2 | 0.8–0.9 |

So the app's cost splits roughly into **~8 points fixed** (present + weave, which
run per panel refresh regardless of how often the app submits), **~4 points that
scale with submission rate**, and **~2 points of actual model render**. That is
the whole budget: even at 5 Hz the floor is ~8.5 %.

dwm sits at 10.7–13.8 % (typically ~12.7 while playing, ~11 idle) and is stable
across every renderer config — the largest single consumer, and outside this repo.

## Results

| Item | Change | Result |
|---|---|---|
| 1 | Render targets sized to the viewport, not the zone swapchain | **1708 MB → 20 MB** |
| 2 | MSAA 8× → 2× | 0.45 → 0.26 ms/view; quality signed off by eye |
| 3 | Per-view `vkQueueWaitIdle` → UBO ring + per-frame fence | **+40 % throughput** (85 → 119 views/s) |
| 4 | Silhouette 640×360 → 256×144, scoped fence | small; see below |
| 5 | Feather pass scissored to a 4-strip border band | byte-identical, no measured cost |
| 7 | Idle throttling gated on `isTracking` | **−40 % app GPU** (paired, 4 reps) |
| 8 | `DXR_AVATAR_SKIN_HZ` cap | CPU/upload only, default off |
| 9 | `DXR_ZONE_VIEW_SCALE` supersample trim (runtime-side) | **−3 %** app GPU — *was recorded as −15.7 %* |

### End to end

Three configs, interleaved within each rep. `A` is the *knob-reachable* baseline,
not true pre-work: item 3's UBO ring has no revert knob and it raised throughput,
so `A` reads slightly higher than the original stalled build did.

| Config | | clip playing | clip paused (idle, no viewer) |
|---|---|---|---|
| A | MSAA 8×, swapchain-sized targets, no throttle | 16.1 | 16.3 |
| B | shipping defaults (MSAA 2×, viewport targets, throttle) | 15.2 | **9.8** |
| C | B + `DXR_ZONE_VIEW_SCALE=0.5` | 14.7 | **8.9** |

Model render across those: 2.9–3.4 % → 1.5–2.2 % → 0.7–1.1 %.

**Single-digit is reached only by throttling**, and only with the clip paused and
no viewer tracked (throttle drops to `DXR_AVATAR_AWAY_HZ`, 5 Hz). Repeated: 8.50,
9.22, 9.59, 9.71, 9.95 across configs and reps. With the clip playing, nothing
here reaches single digits, because ~12 of the app's ~15 points are present +
weave + blits rather than render.

### Item 9 — corrected

Isolated B-vs-C, 5 reps, discarding the one pair whose two runs sat in different
states (see the intermittent state in [traps](#measurement-traps)):

| rep | app GPU B → C | render B → C |
|---|---|---|
| 1 | 15.23 → 15.01 (−1.4 %) | 1.97 → 1.02 |
| 2 | 22.86 → 22.51 (−1.5 %) | 2.20 → 1.08 |
| 3 | 15.60 → 14.78 (−5.3 %) | 1.93 → 1.06 |
| 5 | 14.92 → 14.31 (−4.1 %) | 1.82 → 0.90 |

So ≈ **−0.5 points (−3 %)**, not −15.7 %. The fill saving itself is real and large
— the trim halves the renderer's own cost — but the renderer is only ~2 of ~15
points, so halving it cannot move the total much. The original prediction of 1–3 %
was right; the −15.7 % was a measurement artifact.

Since item 9 is the one change that costs image quality, −3 % does not justify
turning it on by default. It stays opt-in, and the pending "eyeball 0.5 and some
intermediate values" call is no longer worth making for its own sake.

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

Not ours, but load-bearing for any measurement here: `LEIA_DP_DISABLE_BG_CAPTURE`
(plug-in side) — see the chroma-key note below.

## Measurement

Use **`\GPU Engine(*)\Running Time`**, not `Utilization Percentage`.

`Running Time` is a monotonic counter in 100 ns units, so a start/end delta
integrates the whole window exactly. `Utilization Percentage` is an instantaneous
gauge, and `Get-Counter` with a wildcard instance costs ~2.5 s per call — a 15 s
run therefore averaged ~7 samples of a fluctuating value, and the same config
landed anywhere from 11.3 % to 23.4 %. On the counter method the same config
repeats to ±0.6 points across 8 consecutive launches.

Unit proof rather than assumption: read as microseconds the counter puts a single
dwm engine at 117 %, which is impossible; ÷10 puts it at 11.8 %. Hence 100 ns.

Take the interval from the counter samples' own `Timestamp`, not `Get-Date`, or
the ~2.5 s each snapshot takes skews a short window.

Break the counter out **per luid** — `pid_*_luid_*_engtype_*`. This box has an Arc
iGPU and a 4070, adapter selection has been nondeterministic here before, and
summing luids together would hide a switch. (8/8 launches stayed on the iGPU, so
it was not the cause of the swing below — but the check is cheap.)

Harness in the session scratchpad: `combined_busy.ps1` (interleaved A/B/C on busy
time, `-Only` to isolate one item), `luid_probe.ps1`, `bgcap_ab.ps1`,
`winrect.ps1`, `kill_avatar.ps1`.

## Measurement traps

- **An intermittent state adds ~8 points to `app3d`, in roughly 1 launch in 4.**
  Same binary, same env, same window rect (463×812, verified), same adapter, same
  `tracked%`, and the renderer's own timestamped ms/view is *unchanged* — so it is
  extra non-render work in the app process, cause not yet identified. It is
  visible in-run: `app3d` ≈ 19–21 instead of ≈ 12. **Pair configs only when both
  runs are in the same state**, and repeat enough to see which. A pair split
  across the two states manufactures a ±40 % result out of nothing — that is where
  the −15.7 % for item 9 came from.
- **Nothing is at steady state for the first ~4.5 s.** Eye tracking has not
  engaged, so both eyes sit on the SR nominal viewer and render *identically*. A
  one-shot capture or matrix dump inside that window shows collapsed stereo that
  is not real. This produced a wrong "the panel isn't showing 3D" conclusion.
- **Do not eyeball atlas halves.** Two byte-identical views read to the eye as
  two different head angles. Diff them numerically (mean abs RGB; ~20 with
  disparity, 0.00 without).
- **Log `tracked%` during any throttle measurement.** Whether a face is in front
  of the panel decides between `DXR_AVATAR_IDLE_HZ` (15) and `DXR_AVATAR_AWAY_HZ`
  (5), which is a 3× difference in submission rate on its own. It is not
  controllable from the harness — it depends on whether someone is sitting there.
- **ms/view is not comparable across frame rates.** At ~10 views/s the iGPU
  downclocks and per-view time rises ~5× (0.17 → 0.93 ms) even though total GPU
  work falls. Compare ms/view only between runs at similar rates.
- **MSAA A/B cannot be pixel-compared** while head tracking moves the camera
  between runs. Compare edge character, not pixels.
- `DXR_AVATAR_GPUTIME` adds a per-view fence wait, so absolute frame rates from a
  GPUTIME run are slightly pessimistic. Fine for comparing configs.

## Side finding: the transparency path is cheap, its fallback is not

`LEIA_DP_DISABLE_BG_CAPTURE=1` makes the plug-in fall back from WGC background
capture to chroma-key, and app GPU **rises** from ~18 % to ~39 % (paired, 3 reps;
views/s 116 → 105). Whatever else is true of the transparency path, WGC capture is
not the expensive part of it, and this env var is not a perf win despite reading
like one.

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

- **Item 6 — render directly into the runtime swapchain image.** Ceiling is **well
  under 1 point**; not worth the refactor. Two measurements bound it:
  - The `renderEye` timestamps bracket everything *including* the final
    `vkCmdBlitImage`, and that whole per-view total is only **1.9 points** at MSAA
    2× / full tile. The blit is a part of that, not an addition to it.
  - Quartering the tile area (`DXR_ZONE_VIEW_SCALE=0.5`) takes that total 1.97 →
    1.02, so **all** area-proportional per-view work — render + feather + blit —
    is ~1.3 points combined. Removing only the blit saves a fraction of that.

  The copy engine's steady 3.4–3.6 points is **not** this blit: it is unchanged
  when the tile is quartered (3.54→3.57, 3.59→3.59, 3.33→3.27), so it is a
  fixed-size per-frame cost — the weave/present at panel resolution — and it is
  submission-scaled only in the sense that it happens once per frame. A
  graphics-queue blit lands on the 3d engine anyway, not the copy engine.

  Against that, the change needs per-swapchain-image framebuffers and the MSAA
  resolve retargeted at the swapchain, in the code path where removing a
  synchronisation mistake already silently collapsed stereo once (see landmines).
  Not a good trade.
- **Item 4's depth/alpha-only silhouette pipeline.** Measured silhouette cost is
  ~0.3 % of GPU, which does not justify a second pipeline + shader + render pass.
- **Root cause of the intermittent +8-point state.** Ruled out: adapter switch,
  window geometry, viewer presence, background capture, renderer config, GPU
  clocks (the renderer's own ms/view does not rise with it). Worth a GPUView or
  PresentMon trace next.

## Open question for whoever picks this up

"Single-digit iGPU" is met only with the clip paused and no viewer tracked. With
the clip playing the app sits at ~15 %, and ~12 of those points are present +
weave + blits, not render. Below that means the weave and dwm (~12 %), neither of
which is in this repo. Worth settling which reading the 33 % refers to — the
avatar process's Task Manager column, or the system total. On this box those are
~15 % and ~30 %.
