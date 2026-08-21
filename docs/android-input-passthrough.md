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
