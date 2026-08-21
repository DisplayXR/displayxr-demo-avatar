# Android input punch-through — measured, and why it is blocked (#66)

**Verdict: on stock Android 13 a DisplayXR Architecture-A app cannot let taps
reach the desktop behind it. Not "hard", not "needs a hack" — blocked by design,
three times over.** Everything here was measured on the reference NP02J
(Android 13, SDK 33) against the `avatar_vk_android` build on `main`.

## What we wanted

The avatar is a full-screen, fully transparent window with a small tiger in it.
The desktop is plainly visible through the other 95% of it — and completely
dead: every tap lands on the avatar. The goal was the obvious one: tap a
launcher icon *through* the transparent pixels, while taps on the tiger still
drive the avatar.

## Baseline (the bug)

```
avatar up   → adb shell input tap 430 1006   → nothing happens
avatar down → adb shell input tap 430 1006   → App Center opens
```

The avatar's input window confirms it — its touchable region is the whole panel:

```
name='… avatar_vk_android/.MainActivity', inputConfig=0x0, alpha=1.00,
frame=[0,0][1600,2560], touchableRegion=[0,0][1600,2560],
ownerUid=10482, touchOcclusionMode=BLOCK_UNTRUSTED
```

## Wall 1 — the per-region touchable API is blocklisted

The textbook fix is the floating-head technique: declare a touchable region
smaller than the window via `ViewTreeObserver.OnComputeInternalInsetsListener` +
`InternalInsetsInfo.touchableRegion` with `TOUCHABLE_INSETS_REGION`. It is
`@hide`, so it has to be reflected. On device:

```
W Accessing hidden method ViewTreeObserver$InternalInsetsInfo;->setTouchableInsets(I)V
    (unsupported, reflection, allowed)
W Accessing hidden field  ViewTreeObserver$InternalInsetsInfo;->touchableRegion:…
    (max-target-r, reflection, DENIED)
W avatar: #66 punch-through unavailable — the whole window stays touchable
    java.lang.NoSuchFieldException: touchableRegion
```

The setter is merely unsupported; the **field is denied** — `max-target-r` means
only apps targeting API ≤ 30 may touch it, and we target 31. So the region can
never actually be filled in. (Dropping to targetSdk 30 to dodge this would be a
dead end anyway — see walls 2 and 3.)

## Wall 2 — untrusted-touch occlusion reads the FRAME, not the region

Even with a region, Android's anti-tapjacking check asks whether an *obscuring*
window's **frame** contains the touch point; the obscuring window's touchable
region is not consulted. And an Activity window is
`touchOcclusionMode=BLOCK_UNTRUSTED` (measured above) — a hard block that no
window alpha softens. Only `TYPE_APPLICATION_OVERLAY` windows get `USE_OPACITY`,
which is the mode where the ≤ 0.80 alpha escape applies. On this device
`maximum_obscuring_opacity_for_touch = 0.8`, and the only `USE_OPACITY` window
present is the OEM's `FloatAssist` overlay — everything else, ours included, is
`BLOCK_UNTRUSTED`.

So the geometry lever is the only one left: make the frame itself smaller. That
works — `applySlabWidth()` set a centred 55%-width slab and the frame genuinely
moved:

```
frame=[0,576][1600,1984]        # was [0,0][1600,2560]
touchableRegion=[0,0][1600,2560]   # ← still the whole display!
```

…which exposed a prerequisite worth recording: **a touch-MODAL window's
touchable region is the whole display however small its frame is**
(`WindowState.getSurfaceTouchableRegion` widens it unless `FLAG_NOT_TOUCH_MODAL`
or `FLAG_NOT_FOCUSABLE` is set). With that flag added, region finally tracked
frame:

```
frame=[0,576][1600,1984]  touchableRegion=[0,576][1600,1984]
```

## Wall 3 — `ActivityRecordInputSink` (the one that actually kills it)

With a real 55% slab and a frame-matched touchable region, a tap on a launcher
icon outside the slab **still did nothing**: focus stayed on the avatar, the
launcher never reacted, and — the tell — *no* `Untrusted touch due to occlusion`
warning was logged. It was not the opacity policy. `dumpsys input` shows why;
directly below our window and above the launcher sits:

```
13: name='… avatar_vk_android/.MainActivity',      frame=[0,576][1600,1984]
14: name='… ActivityRecordInputSink …/.MainActivity',
        inputConfig=NO_INPUT_CHANNEL | NOT_FOCUSABLE, alpha=1.00,
        touchableRegion=[-24000,-15999][27199,16000]      ← the whole universe
19: name='… QuickstepLauncher',                    frame=[0,0][1600,2560]
```

Android 12L+ parks an `ActivityRecordInputSink` under every Activity precisely to
stop touches reaching a **different-uid** activity below it. It has
`NO_INPUT_CHANNEL`, so the dispatcher finds it as the touched window and drops
the event silently. Its region is display-wide and **independent of our frame**
— shrinking the window cannot shrink the sink. Note the contrast with the same
sink for backgrounded activities, which carries `NOT_TOUCHABLE`; ours does not,
i.e. ours is live.

**Consequence: click-through and Architecture A's own-window are mutually
exclusive on stock Android.** The sink sits *in front of* the untrusted-touch
policy, so even a trusted-overlay or untrusted-touch allowlist exemption would
not be enough on its own for an Activity — the sink has to be scoped too.

## What ships

Everything above is implemented and **inert by default**:

| Piece | Where | Default |
|---|---|---|
| Silhouette → 20 band rects + bubble rect, in canvas px | `main.cpp` `update_silhouette`, `nativeGetTouchRegion` | published (cheap; also documents the intended region) |
| Reflective `TOUCHABLE_INSETS_REGION` listener | `MainActivity.installTouchRegionListener` | installs, fails at wall 1, logs once, degrades to whole-window |
| Frame slab + `FLAG_NOT_TOUCH_MODAL` | `MainActivity.applySlabWidth` | **100 % = off**, no behaviour change |

Two knobs, live, no rebuild:

```bash
adb shell setprop debug.dxr.avatar.slab 55        # frame → 55% centred slab
adb shell setprop debug.dxr.avatar.passthrough 0  # force whole-window touchable
```

They exist because this machinery is the executable form of the OEM ask: on a
platform that grants it, arming punch-through is a `setprop`, not a port.

## The ask, and one correction to it

Tracked as **S2 / L13** in `displayxr-runtime`
(`docs/specs/vendor/oem-android-platform-requirements.md`, issue #1038). The
correction this work forces: S2's mechanism list (trusted-overlay bit,
untrusted-touch allowlist, public per-region API) is **necessary but not
sufficient for Architecture A** — an OEM must *also* scope or disable
`ActivityRecordInputSink` for the designated package.

There is also a second route worth a trade study before anyone reverts #64. The
0.80 ghost that killed the old overlay topology was a consequence of a
*full-screen* overlay: occlusion opacity only accumulates for windows whose
frame contains the tap. An app-owned `TYPE_APPLICATION_OVERLAY` sized to a tight,
screen-relative avatar rect would need **no** alpha reduction — taps outside its
frame are not obscured at all — and it carries no `ActivityRecordInputSink`,
because there is no Activity. That would deliver full opacity *and* click-through
on stock Android, at the cost of `SYSTEM_ALERT_WINDOW`, a keep-alive service, and
the backgrounding dance #64 removed. **Not measured** — stated here as the
hypothesis to test, not as a result.

## What works today

In-app input is unaffected and correct: taps on the tiger drive it, taps
elsewhere in our window are ignored by the gesture gate (`sil_hit`) rather than
misinterpreted. What is blocked is only cross-uid pass-through to the desktop.

---

## Update, 2026-08-21 — the tight overlay works; shrinking it onto Leo does not (#67, reverted)

Two things have since been *measured*, and they point opposite ways.

**The tight-overlay route works** (runtime#1110 §4, shipped in #66). A touchable,
tight, no-foreground-Activity `TYPE_APPLICATION_OVERLAY` gets `alpha=1.00` *and*
click-through with zero grants. Two corrections to the text above: the 0.80 was
**not** only self-imposed — the platform clamps a `FLAG_NOT_TOUCHABLE` app
overlay, so being *touchable* is what kills the ghost; and `moveTaskToBack(true)`
is enough to make the Activity's sink inert, so the Activity need not be gone.

**Per-pixel shaping is still dead.** `TOUCHABLE_INSETS_REGION` is blocklisted for
**overlay windows too** — the blocklist is per-API, not per-window-type. An
overlay's touchable region is exactly its **frame**.

That leaves only "shrink the frame onto the silhouette", which #67 implemented and
which was **reverted the same night**. It is worth recording precisely why,
because the failure is structural and any retry will hit it again.

### Width is NOT free — the speech bubble puts it back in the loop

#67's safety argument was that only the HEIGHT axis feeds back. That is true *for
Leo*: `render_frame` chains `XrDisplayRigDXR.virtualDisplayHeight = rig_vh`, and
`rig_vh` comes from the **model's** extent, so metres-per-pixel is
`rig_vh / zone_height` — a function of height alone. Leo really is invariant under
a width change (measured: a 1200 → 800 px frame moved his bbox from
`308,460 600x1096` to `108,464 596x1092`).

The argument does not cover the **speech bubble**, and the bubble is the whole
problem. The bubble is a Local2D layer whose rect is a straight fraction of the
canvas **pixel width**:

```c
const uint32_t cw = g_win_px_w.load(...);          // the overlay window's px width
float bwf = (float)cw * (0.70f * 2.0f / 3.0f);     // = 0.4667 * cw
float bhf = bwf * bubble_aspect;                   // aspect = 384/1024
```

So the bubble is (a) **sized from** the frame width and (b) **an input to** the
width driver, which unions its rect in. Both halves of a feedback loop. Solving
the bubble-only fixed point of `want = 2*(0.4667*cw/2 + 0.05*curH)` gives
`cw ≈ 300` — it collapses to the `kShapeMinFrac` floor. Only Leo's bbox holds the
frame open, so on any frame where `g_sil.bbox_valid` is false — including the
surfaceChanged transient the resize itself causes — the frame dives.

### What that produced on the reference NP02J

With grow-immediate / shrink-held-1 s, the loop does not settle, it **oscillates**.
Measured over 24 s of an idle Leo, 10 applied resizes:

```
1200 -> 736 -> 896 -> 992 -> 1088 -> 768 -> 928 -> 1056 -> 896 -> 992 -> 1088
```

Three user-visible symptoms, all one storm:

* **Flicker.** Every applied width change is `surfaceChanged` → new buffer queue →
  swapchain recreate. A screencap taken mid-oscillation shows Leo **entirely
  absent**. The "one resize, one hitch" reasoning held for a single resize, not a
  stream of them.
* **Minuscule speech bubble.** Direct first-order consequence of `bw = 0.4667*cw`:
  1200 px → 560 px wide, 832 px → 388 px, and at the 408 px floor → **190 px**,
  under a tenth of the original area.
* **Black fringes.** `AG_GEO` re-derives strip + framebuffer + viewport from the
  frame on every change (`target=1200x1600` → `736x1600` → `896x1600` → …). The T2
  backdrop re-crop is asynchronous, so between the resize and the re-crop the gate
  runs against a stale backdrop and the fresh swapchain edges read black. Because
  the frame never stops moving, that transient is permanent.

### If this is retried

The bbox driving the frame must be **pose-independent** — the model's scene-space
footprint at the current dolly, which changes only on a deliberate user
interaction — not a live projection that jitters with head tracking, and not
anything that is itself sized from the frame. Concretely: give the bubble a
width-invariant size (derive it from the canvas HEIGHT, which is already fixed, or
from a scene-space extent) *before* re-attempting any width shaping, and damp
GROW as well as SHRINK so no single jitter spike can trigger a resize.

True per-pixel shaping still needs the consent-gated AccessibilityService route
designed in displayxr-runtime#1114.
