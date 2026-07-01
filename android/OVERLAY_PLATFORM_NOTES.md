<!--
Copyright 2026, The DisplayXR Project and its contributors
SPDX-License-Identifier: Apache-2.0
-->

# Android overlay-mode: platform idiosyncrasies & workarounds (#558)

This document explains the platform constraints behind "overlay mode" — the
3D, head-tracked avatar that floats **over the live, interactive launcher** —
and the workarounds we ship because **DisplayXR is an ordinary installed app,
not a signed platform/system app**.

If a partner **owns the device** (platform signing key, system-app install,
device-owner/DPC, or OEM ROM control), almost every workaround below collapses
into a one-time provisioning step. The [**"If you own the device"**](#if-you-own-the-device)
section maps each hack to its clean platform-level equivalent.

Tested on **nubia Pad 3D II (NP02J)**, MyOS 13 / Android 14.

---

## 1. The two-process model

Overlay mode involves **two** unprivileged apps:

| Process | Package | Role |
|---|---|---|
| The avatar app | `com.displayxr.avatar_vk_android` | OpenXR client; renders the tiger; hosts the **input** overlay |
| DisplayXR runtime (OOP) | `org.freedesktop.monado.openxr_runtime.out_of_process` | Compositor + Leia DP; owns the **visual** weave overlay surface |

The runtime runs out-of-process (ADR-025). It creates the on-screen
`TYPE_APPLICATION_OVERLAY` that the avatar's frames are weaved into; the avatar
itself goes to the background (`moveTaskToBack`) so the launcher stays the
interactive foreground app. **Both** processes therefore need "draw over other
apps", and neither is privileged — hence this document.

---

## 2. Required grants (the full dev recipe)

```bash
RT=org.freedesktop.monado.openxr_runtime.out_of_process
PKG=com.displayxr.avatar_vk_android

# (1) Draw-over-other-apps for BOTH processes (neither is a system app):
adb shell appops set $RT  SYSTEM_ALERT_WINDOW allow   # runtime draws the weave overlay
adb shell appops set $PKG SYSTEM_ALERT_WINDOW allow   # app draws the touch input overlay

# (2) Exempt the avatar from the OEM background freezer (see §3.2):
adb shell dumpsys deviceidle whitelist +$PKG
adb shell cmd appops set $PKG RUN_ANY_IN_BACKGROUND allow

# (3) Dev switches (overlay behaviour is opt-in; not a product gate):
adb shell setprop debug.dxr.overlay     1   # overlay mode (app backgrounds + render-while-bg)
adb shell setprop debug.dxr.transparent 1   # translucent weave surface (see-through)
adb shell setprop debug.dxr.mode        1   # LeiaSR 3D weave
```

The app **also** requests grants (1)+(2) at runtime, but only **once ever**
(persisted in `SharedPreferences` `avatar_overlay`/`perms_requested`) — see §3.5.

---

## 3. Workarounds, and *why* (because we're not a system app)

### 3.1 "Draw over other apps" for two unprivileged apps
A system/platform app gets `SYSTEM_ALERT_WINDOW` implicitly (or can use a
higher, non-app overlay window type). As ordinary apps we must declare
`SYSTEM_ALERT_WINDOW` in the manifest and have the user grant it for **both**
the runtime and the app. The app's input overlay is `TYPE_APPLICATION_OVERLAY`
(the only overlay type available to non-system apps since Android 8).

### 3.2 OEM background "freezer" suspends the backgrounded avatar
This ROM's **`CpuFreezerManagerServiceV2`** freezes a backgrounded process
(`/sys/fs/cgroup/uid_<n>/.../cgroup.freeze=1`) **even with a foreground
service**, which halts the render/eye-tracking loop → the tiger stops following
the viewer. There is no app-level API to opt out of an OEM freezer. The only
lever an unprivileged app has is the **doze/battery-optimization allowlist**,
which on this ROM also lifts the freeze:
- dev: `dumpsys deviceidle whitelist +<pkg>`,
- product: the app requests `ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS`.

We also run a `FOREGROUND_SERVICE_TYPE_SPECIAL_USE` keep-alive service
(`OverlayKeepAliveService`) to hold the process foreground — necessary but **not
sufficient** on this ROM (the freezer ignores the FGS; the allowlist is what
matters).

### 3.3 Anti-tapjacking alpha clamp on the pass-through weave overlay
The runtime's full-screen weave overlay uses `FLAG_NOT_TOUCHABLE` so taps pass
through to the launcher. Android clamps a `NOT_TOUCHABLE` overlay from a
non-system app to **≤ 0.80 opacity** (anti-tapjacking), so the weave is slightly
dimmed. A platform-signed/system app is **not** clamped (full opacity).

### 3.4 No per-region touchability → a separate small input overlay
We want the launcher usable *except* on the tiger (touch the tiger to drag it,
tap elsewhere to use the launcher). The clean way is per-region touchability via
`ViewTreeObserver.addOnComputeInternalInsetsListener` /
`InternalInsetsInfo.touchableRegion` — but those are **`@hide`**, and this ROM
**blocks the `VMRuntime.setHiddenApiExemptions` reflection bypass**
(`NoSuchMethodException`). So the full-screen weave overlay is forced to be
all-or-nothing touchable, and we keep it fully pass-through (`FLAG_NOT_TOUCHABLE`).

**Workaround:** the *app* hosts its own **small, touchable** `TYPE_APPLICATION_OVERLAY`
(a plain `View`, so no 0.80 alpha clamp) sized each frame to the tiger's
**silhouette bounding box** (`OverlayKeepAliveService`). On each finger-down it
asks native `nativeHitTiger()`; a tap that misses the actual tiger pixels is
**not consumed** (`onTouch` returns `false`) so it falls through to whatever is
under the box. Touches use **raw display coordinates** because the window itself
moves every frame to follow the tiger (window-local coords would jump under a
stationary finger).

### 3.5 Permission dialogs nagged and got blocked by our own overlay
Re-requesting the battery-exemption every launch (a) annoyed the user and
(b) the system dialog popped up **under** our touchable input overlay, which ate
the "Allow" tap. Fix: request both grants **at most once ever**
(`SharedPreferences`). A granted user is never asked again; a user who declined
can still enable both in Settings.

### 3.6 Runtime "stopped" state on a cold launch
After a force-stop/fresh-install the runtime package is in Android's *stopped*
state, so the Khronos loader's broker lookup excludes it →
`XR_ERROR_RUNTIME_UNAVAILABLE`. We send an explicit intent with
`FLAG_INCLUDE_STOPPED_PACKAGES` to wake it before `xrCreateInstance`. (A real
deployment where the runtime has run at least once doesn't hit this.)

### 3.7 Overlay lifecycle is ours to manage
The runtime keeps the service overlay surface process-wide while a session is
live. On a clean exit (`onTaskRemoved` → stop the FGS → process reaped → IPC
client disconnects → the runtime service goes idle), the runtime tears the
overlay down from its **service `onDestroy`** (`MonadoImpl.shutdown` →
`nativeDestroyServiceOverlay`). Two Android subtleties make this fragile and are
worth knowing if you re-touch it: removal must be **synchronous on the UI
thread** via `WindowManager.removeViewImmediate()` (plain `removeView()` only
schedules a traversal that never runs while the looper is shutting down → frozen
frame); and an IPC-thread teardown at disconnect *races* this lifecycle, so it
was removed. A *rapid* force-stop→immediate-relaunch can still reuse the overlay
before the service idles (a developer action, not a normal quit).

### 3.8 Vendor services are system apps we cannot patch
The LeiaSR device-config service (`com.leialoft.display.config`,
`/system/app`) and the head-tracking service are **system apps** — they can't be
reinstalled without root. They did **not** need patching for #558, but it means
all vendor-side fixes have to live in the **plug-in** (`displayxr-leia-plugin`),
not in those services.

---

## If you own the device

A partner who controls the device can make all of the above "just work". In
rough order of effort:

### A. Pre-grant the permissions (no code changes, no signing)
Via MDM / device-owner (DPC) or a provisioning script, grant once and the app
never prompts:
```bash
appops set <runtime-pkg> SYSTEM_ALERT_WINDOW allow
appops set <app-pkg>     SYSTEM_ALERT_WINDOW allow
dumpsys deviceidle whitelist +<app-pkg>            # or the OEM's battery-allowlist API
cmd appops set <app-pkg> RUN_ANY_IN_BACKGROUND allow
```
A **device-owner** app can call `setPermissionGrantState` /
`setApplicationRestrictions` to do this programmatically at enrollment.
→ removes the §3.1 and §3.5 prompts entirely.

### B. Whitelist from the OEM freezer at the ROM level
Add both packages to the OEM's protected/never-freeze list
(`CpuFreezerManagerServiceV2` config, or the vendor "protected apps" allowlist).
→ removes §3.2 (no battery-exemption hack, no keep-alive-FGS dependency for
correctness).

### C. Install as **privileged system apps** (`/system/priv-app` + permission allowlist)
With the runtime (and optionally the app) as a privileged system app:
- `SYSTEM_ALERT_WINDOW` is granted by default,
- the **anti-tapjacking 0.80 alpha clamp is lifted** → full-opacity weave (§3.3),
- background-execution restrictions are relaxed.

### D. Sign with the **platform key** (or grant `signature`/`signatureOrSystem` perms)
A platform-signed app can:
- use **hidden APIs** → real **per-region touchability**
  (`touchableRegion`), so the *single* full-screen weave overlay can be
  "touchable on the tiger, pass-through elsewhere" — the **separate input
  overlay (§3.4) becomes unnecessary**,
- use higher/system overlay window types,
- access OEM/vendor SDKs without the hidden-API gate.

### E. OEM ROM integration (the cleanest)
Bake DisplayXR into the system image: pre-granted permissions, freezer
allowlist, full-opacity touchable overlay, and the vendor display/head-tracking
services co-versioned with the runtime. At that point overlay mode needs **none**
of the §3 workarounds — they exist purely because we run as an unprivileged,
unsigned, sideloaded app today.

### What stays regardless of privilege
- The **two-process** split (app + OOP runtime) is architectural (ADR-025), not
  a workaround.
- The **silhouette hit-test** for tiger-vs-launcher touch routing is desired
  behaviour; only its *implementation* (separate input window vs per-region
  touchability) changes with privilege.

---

## Dev switches (system properties)

| Property | Effect |
|---|---|
| `debug.dxr.overlay` | Overlay mode: app backgrounds + renders with no window; runtime keeps the OOP session alive; plug-in keeps 3D while backgrounded |
| `debug.dxr.transparent` | Translucent weave surface (see-through over the launcher) |
| `debug.dxr.mode` | `1` = LeiaSR 3D weave |

`debug.dxr.overlay` is the master switch read by all three components (app,
runtime, plug-in). Off = a normal foreground 3D app.

## Build & run quick reference

```bash
# Avatar APK — needs JDK 21 (NOT the JDK 11 used for CNSDK):
JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-21.jdk/Contents/Home \
  ./gradlew :android:assembleDebug
adb install -r android/build/outputs/apk/debug/android-debug.apk

# Then apply the grants + setprops from §2 and launch:
adb shell am start -n com.displayxr.avatar_vk_android/.MainActivity
```

Cold start ~15–20 s. Screen-capture is **blind to the VK overlay** — verify on
the physical panel.

## Related

- Runtime side: `displayxr-runtime` (service overlay, session keep-alive,
  overlay teardown on disconnect).
- 3D-while-backgrounded: `displayxr-leia-plugin` branch
  `feat/android-overlay-3d-558`.
