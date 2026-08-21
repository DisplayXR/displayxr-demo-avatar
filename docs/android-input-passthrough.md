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

## Update, 2026-08-21 — the residual bounding box, and how it shrank (#67)

Both hypotheses above have since been *measured*, and the picture changed:

* The tight-overlay route **works** (runtime#1110 §4, shipped in #66): a
  touchable, tight, no-foreground-Activity `TYPE_APPLICATION_OVERLAY` gets
  `alpha=1.00` *and* click-through with zero grants. Two corrections to the text
  above: the 0.80 was **not** only self-imposed — the platform clamps a
  `FLAG_NOT_TOUCHABLE` app overlay, so being *touchable* is what kills the ghost;
  and `moveTaskToBack(true)` is enough to make the Activity's sink inert, so the
  Activity need not be gone.
* Per-pixel is still dead: `TOUCHABLE_INSETS_REGION` is blocklisted for **overlay
  windows too** — the blocklist is per-API, not per-window-type. So an overlay's
  touchable region is exactly its **frame**, and "shape the region to the
  silhouette" can only mean "shrink the frame onto the silhouette".

That is what #67 does, and the interesting part is why it is safe.

### Width is free; height is not

Under Architecture A the frame **is** the canvas, so resizing it feeds straight
back into what we render — the collapse this repo already documents for
`canvasRectPx`. But the two axes are not alike:

`render_frame` chains `XrDisplayRigDXR.virtualDisplayHeight = rig_vh`, and
`rig_vh` comes from the **model's** extent, never from the canvas. The runtime
frames the zone rect to that many metres of world height, so metres-per-pixel is
`rig_vh / zone_height`:

* shrink the **height** → Leo shrinks by the same factor → his bbox shrinks →
  the next height shrinks → collapse. **Never touched.**
* shrink the **width** at constant height → only the horizontal extent of the
  off-axis frustum changes. Leo keeps his pixel size *and* his position on the
  panel.

Measured on the reference NP02J, 1200 px → 800 px frame width:
`silhouette bbox: 308,460 600x1096` → `108,464 596x1092`. Leo is invariant to
within the ~10 Hz sampling jitter.

The driver is also invariant under its own output. The frame is centred, so a
width change of `dw` moves the origin by `-dw/2` and grows the canvas by `dw`;
the half-extent measured from the frame **centre**, `|bbox_edge - curW/2|`, does
not move. It converges in one step rather than asymptotically, and cannot walk
the frame off Leo. The speech bubble is unioned in (it is ours to tap) and is a
contraction too — its own width is ~0.47 of the canvas, so it can never demand a
frame wider than itself.

### What it costs, and the damping

Every applied change is a `surfaceChanged` → new buffer queue → swapchain
recreate, i.e. a visible hitch. So the policy is deliberately sticky: **grow
immediately** (never clip Leo, even for a frame), **shrink only** after the
narrower value has held a second, and never resize twice inside 900 ms. In
practice it settles once, a second or so after launch, and then stays put.

Two traps found while building it:

* `onDisplayChanged` fires for **refresh-rate** switches, and this panel hops
  60/90/120/144 Hz on its own. Resetting the shaper there bounced the frame
  800→1200→800 every six seconds. It now resets only when the reference extent
  really moved.
* A plain rotation is deliberately **not** a reset: `overlayExtent()` keys off
  the panel's short edge, so the frame — and Leo in it — is the same size in both
  orientations, and the settled width is still right.

### Result

Reference NP02J, portrait: frame `[0,680][1600,1880]` → `[384,960][1216,2560]`,
i.e. 1200×1600 → 832×1600, **31 % less screen eaten**, and the horizontal dead
margin around Leo drops from ~600 px to ~230 px. `alpha=1.00`, `USE_OPACITY`, no
`ActivityRecordInputSink`. A tap at panel (210, 1645) — inside the old band, dead
before — opens App Center with no `Untrusted touch` line; a drag on Leo still
drives Leo and the launcher does not see it.

The knob, live, no rebuild:

```bash
adb shell setprop debug.dxr.avatar.overlay.shape 0   # back to the full band
```

### What is still eaten, and the only route to per-pixel

The residual is the **union rectangle** of Leo ∪ the bubble, so the transparent
corners inside that rectangle still swallow taps. One rect cannot do better,
because the platform gives an overlay exactly one touchable rect.

Getting true silhouette precision on stock needs a second, consent-gated
mechanism: an in-app `AccessibilityService` that re-dispatches a tap the app
knows landed on a transparent pixel (`sil_hit` already answers that question,
from scene geometry, with no readback). Designed and tracked in
displayxr-runtime#1114. It costs a one-time accessibility toggle by the user and
~50-100 ms of forwarded-tap latency, which is why it is a *tier B* opt-in rather
than the default.
