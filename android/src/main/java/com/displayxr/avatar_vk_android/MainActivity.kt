// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// Thin NativeActivity wrapper for the avatar demo. Architecture A (in-process,
// ADR-036 D2 / runtime#1063): this app owns the window the runtime weaves into.
// Four jobs:
//   1. Make our OWN window translucent — window.setFormat(PixelFormat.TRANSLUCENT)
//      on top of Theme.Avatar.Transparent. A NativeActivity's Surface otherwise
//      comes up opaque RGBX and SurfaceFlinger discards our alpha whatever the
//      theme says, so this one line is what makes the launcher show through.
//   2. Publish this window's on-screen rect to native every frame (Choreographer)
//      so the runtime can anchor the weave phase + the per-window Kooima canvas
//      where the window physically sits on the panel (ADR-036 D6).
//   3. Push the authoritative 4-way display rotation to native on launch and on
//      every rotation (incl. 180° flips, via a DisplayListener) — the renderer
//      can't derive true rotation from its own surface, and
//      Configuration.orientation only distinguishes portrait/landscape.
//   4. Forward touch to native via dispatchTouchEvent — a NativeActivity's
//      native input queue is NOT fed by dispatchTouchEvent. One finger = camera
//      controls (depth dolly + lateral strafe), two fingers = pinch-zoom,
//      double-tap = recenter. Plus: wake the DisplayXR runtime out of Android's
//      "stopped" state before xrCreateInstance, and prompt if unreachable.
//   5. #66 input punch-through (PER-PIXEL) — INERT ON STOCK ANDROID 13, and the
//      touch-region section below records exactly why. Stock blocks it, so the
//      machinery ships disabled and re-arms itself on a platform that grants the
//      S2/L13 exemption. It applies to the Activity-window path below.
//   6. runtime#1110 CLICK-THROUGH OVERLAY MODE (PER-FRAME) — the DEFAULT once the
//      user has granted "display over other apps". The window we present into is
//      then not this Activity's but a tight, full-opacity, TOUCHABLE
//      TYPE_APPLICATION_OVERLAY added through WindowManager, with the Activity
//      moved OUT of the foreground task. That combination is what makes the
//      desktop around Leo tappable again on STOCK Android 13 — see the note at
//      `startOverlayMode` for which of job 5's three walls each property clears.
//      Everything #1063 bought survives: window type and handoff class are
//      orthogonal axes, and XR_DXR_android_surface_binding takes any
//      ANativeWindow. Without the grant the app stays on this Activity's window:
//      still renders, still weaves, just swallows taps.
//
// Vendor-neutral: this APK carries zero CNSDK classes and zero vendor .so. It
// does hold CAMERA, because in-process the vendor face tracker runs here.

package com.displayxr.avatar_vk_android

import android.app.AlertDialog
import android.app.NativeActivity
import android.content.Context
import android.content.Intent
import android.content.res.Configuration
import android.graphics.PixelFormat
import android.graphics.Point
import android.graphics.Region
import android.hardware.display.DisplayManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.view.Choreographer
import android.view.Gravity
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.Toast

class MainActivity : NativeActivity() {

    companion object {
        // Load the native lib into the JVM so the external JNI functions below
        // resolve (NativeActivity also dlopens it for android_main; this load
        // is what binds the Java_… symbols).
        init {
            System.loadLibrary("avatar_vk_android")
        }

        // Runtime flavors, in discovery preference order. ADR-025: the
        // out-of-process runtime is production; in_process is dev-only.
        private val RUNTIME_PACKAGES = arrayOf(
            "org.freedesktop.monado.openxr_runtime.out_of_process",
            "org.freedesktop.monado.openxr_runtime.in_process",
        )

        private const val TAG = "avatar"
        // #66 touch region: 20 silhouette bands + the bubble, plus slack.
        private const val MAX_TOUCH_RECTS = 32
        // Ignore sub-fingertip jitter so an animating tiger doesn't ask for a
        // layout traversal every single frame.
        private const val REGION_EPS_PX = 8
    }

    // Implemented in main.cpp. rotation = Surface.ROTATION_0/90/180/270 → 0/1/2/3.
    private external fun nativeSetRotation(rotation: Int)

    // True once xrCreateInstance failed with RUNTIME_UNAVAILABLE.
    private external fun nativeRuntimeUnavailable(): Boolean

    // True once the OpenXR instance is up (runtime reached).
    private external fun nativeXrReady(): Boolean

    // ADR-036 D6 / runtime#1033: this window's on-screen rect (origin + size),
    // the raw panel extent, and the display id. The runtime needs all of it to
    // anchor the weave phase and the per-window Kooima canvas.
    private external fun nativeSetWindowRect(
        x: Int, y: Int, w: Int, h: Int, panelW: Int, panelH: Int, displayId: Int,
    )

    // Touch bridge: one finger = camera controls (depth dolly + lateral strafe),
    // two-finger pinch = zoom (all handled native-side). We own the window now,
    // so touch lands here directly.
    private external fun nativeOnTouch(
        action: Int, count: Int, x0: Float, y0: Float, x1: Float, y1: Float,
    )

    // Double-tap recenters the camera (pan/dolly/zoom back to the framed default).
    private external fun nativeResetView()

    // #66: the tiger silhouette (as horizontal bands) + the speech bubble, in
    // WINDOW px, written into `out` as flat x,y,w,h quads. Returns the rect
    // count, 0 = nothing touchable, -1 = not measured yet (whole window).
    private external fun nativeGetTouchRegion(out: IntArray): Int

    // #66: how wide this window should be, as a percentage of the screen's long
    // edge. 100 = full width (the pre-#66 behaviour).
    private external fun nativeGetSlabPercent(): Int

    // runtime#1110 click-through overlay mode: hand the runtime the Surface of
    // the WindowManager overlay we present into instead of the Activity's.
    private external fun nativeSetOverlaySurface(surface: Surface?)

    // Read an int sysprop through native (android.os.SystemProperties is a hidden
    // API the reflection blocklist refuses at targetSdk 31 — job 5's wall 1).
    private external fun nativeGetIntProp(name: String, def: Int): Int

    // First installed runtime package, preferring out_of_process. Null if none.
    private val installedRuntime: String? by lazy {
        RUNTIME_PACKAGES.firstOrNull {
            try {
                packageManager.getLaunchIntentForPackage(it) != null ||
                    packageManager.getPackageInfo(it, 0) != null
            } catch (_: Throwable) {
                false
            }
        }
    }

    private val gestureDetector by lazy {
        GestureDetector(
            this,
            object : GestureDetector.SimpleOnGestureListener() {
                override fun onDoubleTap(e: MotionEvent): Boolean {
                    try {
                        nativeResetView()
                    } catch (_: Throwable) {
                    }
                    return true
                }
            },
        )
    }

    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        gestureDetector.onTouchEvent(event) // double-tap → recenter
        val n = event.pointerCount
        val x1 = if (n > 1) event.getX(1) else 0f
        val y1 = if (n > 1) event.getY(1) else 0f
        try {
            nativeOnTouch(event.actionMasked, n, event.getX(0), event.getY(0), x1, y1)
        } catch (_: Throwable) {
            // Native lib not bound yet — ignore until it is.
        }
        return super.dispatchTouchEvent(event)
    }

    // Watch native bring-up just until it resolves: if the runtime can't be
    // reached, prompt to launch DisplayXR; if it comes up, stop. Bounded poll.
    private fun watchForRuntimeUnavailable() {
        val handler = Handler(Looper.getMainLooper())
        handler.postDelayed(
            object : Runnable {
                var tries = 0
                override fun run() {
                    if (isFinishing) return
                    val unavailable = try { nativeRuntimeUnavailable() } catch (_: Throwable) { false }
                    if (unavailable) {
                        showRuntimeMissingDialog()
                        return
                    }
                    val ready = try { nativeXrReady() } catch (_: Throwable) { false }
                    if (ready) return
                    if (tries++ < 15) handler.postDelayed(this, 1000)
                }
            },
            2000,
        )
    }

    // In-process (Architecture A) the vendor face tracker opens the front camera
    // in THIS process, so the CAMERA grant is ours to request. Without it the
    // camera open fails silently and every xrLocateViews falls back to the
    // nominal viewer — the tiger simply stops following you, with no error.
    private fun requestCameraOnce() {
        try {
            if (checkSelfPermission(android.Manifest.permission.CAMERA) ==
                android.content.pm.PackageManager.PERMISSION_GRANTED
            ) {
                return
            }
            requestPermissions(arrayOf(android.Manifest.permission.CAMERA), 1)
        } catch (t: Throwable) {
            android.util.Log.w("avatar", "camera permission request failed", t)
        }
    }

    private fun showRuntimeMissingDialog() {
        try {
            AlertDialog.Builder(this)
                .setTitle("DisplayXR not running")
                .setMessage(
                    "Couldn't reach the DisplayXR runtime.\n\n" +
                        "Open the DisplayXR app once (it shows the logo), then reopen this app.",
                )
                .setCancelable(false)
                .setPositiveButton("Open DisplayXR") { _, _ ->
                    installedRuntime?.let { pkg ->
                        packageManager.getLaunchIntentForPackage(pkg)?.let { startActivity(it) }
                    }
                    finish()
                }
                .setNegativeButton("Close") { _, _ -> finish() }
                .show()
        } catch (_: Throwable) {
        }
    }

    // ---------------------------------------------------------------- window rect
    //
    // Choreographer rather than a layout / position listener, because a pure
    // window MOVE produces neither: WindowFrames.didFrameSizeChange compares w/h
    // only, so the move goes out as a `oneway IWindow.moved` that updates
    // mAttachInfo.mWindowLeft/Top and nothing else — no layout, no invalidate, no
    // callback. Meanwhile SurfaceFlinger has already repositioned the layer with
    // the OLD buffer, so an un-updated weave keeps a stale interlace phase for the
    // whole drag and the per-window Kooima frustum stays anchored to the old
    // position. Cost is one getLocationOnScreen per frame plus seven int compares;
    // the native push only happens on an actual change.
    //
    // TRAP: an OEM applying OVERRIDE_SANDBOX_VIEW_BOUNDS_APIS makes
    // getLocationOnScreen return WINDOW-relative coords — every window would report
    // (0,0), silently. The opt-out is the
    // PROPERTY_COMPAT_ALLOW_SANDBOXING_VIEW_BOUNDS_APIS property in our manifest.
    private val locationOnScreen = IntArray(2)
    private var lastRect = intArrayOf(Int.MIN_VALUE, Int.MIN_VALUE, -1, -1, -1, -1, -1)
    private var rectPollRunning = false

    private val rectCallback = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            if (!rectPollRunning) return
            sampleWindowRect()
            sampleTouchRegion()
            Choreographer.getInstance().postFrameCallback(this)
        }
    }

    private fun sampleWindowRect() {
        // In overlay mode the window we present into is the WindowManager overlay,
        // so IT is what has to be sampled. getLocationOnScreen works on any
        // attached view, overlay windows included.
        val view = overlayRoot ?: window?.decorView ?: return
        val w = view.width
        val h = view.height
        if (w <= 0 || h <= 0) return // not laid out yet
        view.getLocationOnScreen(locationOnScreen)
        val display = view.display ?: return
        val real = Point()
        @Suppress("DEPRECATION")
        display.getRealSize(real) // the raw panel extent, not the app bounds
        val next = intArrayOf(
            locationOnScreen[0], locationOnScreen[1], w, h, real.x, real.y, display.displayId,
        )
        if (next.contentEquals(lastRect)) return
        lastRect = next
        try {
            nativeSetWindowRect(next[0], next[1], next[2], next[3], next[4], next[5], next[6])
        } catch (_: Throwable) {
            // Native lib not bound yet — the next frame retries.
        }
    }

    // ---------------------------------------------------------- touch region
    //
    // #66 input punch-through. The avatar is a full-screen transparent window
    // over the live desktop, so it swallows every tap in the 95%-odd of its area
    // that is see-through: the launcher icons are plainly visible and completely
    // dead. Everything below is the machinery to fix that, plus the on-device
    // record of why stock Android 13 will not let it work. Full write-up:
    // docs/android-input-passthrough.md.
    //
    // Input is dispatched at WINDOW granularity — returning false from a touch
    // listener does NOT hand the event to the app behind. The only knob is the
    // window's TOUCHABLE REGION, so we shrink it to the tiger silhouette
    // (published by native as horizontal bands) plus the speech bubble. That is
    // the classic floating-head technique, and it is why the region has to come
    // from the renderer: only native knows where the tiger currently is.
    //
    // MEASURED ON THE NP02J (Android 13 / SDK 33), three independent walls:
    //
    //  1. The API is ViewTreeObserver.OnComputeInternalInsetsListener with
    //     TOUCHABLE_INSETS_REGION, which is @hide. setTouchableInsets() is merely
    //     unsupported ("reflection, allowed"), but the InternalInsetsInfo
    //     .touchableRegion FIELD is "max-target-r, reflection, DENIED" for our
    //     targetSdk 31 — so the region can never be filled in.
    //  2. Even with a region, Android's untrusted-touch rule tests the obscuring
    //     window's FRAME, not its touchable region, and an Activity window is
    //     touchOcclusionMode=BLOCK_UNTRUSTED — which no window alpha can soften
    //     (only TYPE_APPLICATION_OVERLAY gets USE_OPACITY and its <=0.8 escape).
    //  3. And even with the frame shrunk (applySlabWidth below, verified to move
    //     the real frame), Android 12L+ parks an ActivityRecordInputSink directly
    //     beneath every Activity — NO_INPUT_CHANNEL, touchableRegion the whole
    //     display, independent of our frame — expressly to stop touches reaching
    //     a different-uid activity below. It silently ate every tap outside the
    //     slab; the launcher never saw one and nothing was logged.
    //
    // So click-through and Architecture A's own-window are mutually exclusive on
    // stock Android. The machinery stays, gated and default-off, because it is
    // also the executable form of the OEM ask (runtime S2 / L13): on a platform
    // that scopes the sink and grants a trusted overlay, arming it is a setprop.
    //
    // Recomputation only happens inside ViewRootImpl.performTraversals, and a
    // NativeActivity paints straight into its Surface with Vulkan so traversals
    // otherwise never occur. The Choreographer poll that already samples the
    // window rect therefore also diffs the region and calls requestLayout() when
    // it has actually moved; laying out NativeActivity's empty content view is
    // cheap, and the epsilon below keeps that to a few calls per second even
    // while the tiger is animating.
    private val scratchRects = IntArray(MAX_TOUCH_RECTS * 4)
    private val appliedRects = IntArray(MAX_TOUCH_RECTS * 4)
    private var appliedCount = -1           // -1 = whole window (fail-open)
    private var insetsProxy: Any? = null
    private var insetsListenerCls: Class<*>? = null
    private var setTouchableInsetsM: java.lang.reflect.Method? = null
    private var touchableRegionF: java.lang.reflect.Field? = null
    private var touchableInsetsRegionConst = 3  // InternalInsetsInfo.TOUCHABLE_INSETS_REGION

    // #66, the geometry lever: make the window itself narrower than the screen.
    // Android's untrusted-touch check reads the obscuring window's FRAME, so a
    // frame that does not cover a launcher icon stops obscuring it. A "slab":
    // full height, a fraction of the width, centred.
    //
    // VERIFIED to move the real frame (dumpsys input reported [0,576][1600,1984]
    // for 55%), and VERIFIED to be insufficient on stock: ActivityRecordInputSink
    // still swallows everything outside it (wall 3 above). Default is therefore
    // 100 (full width, no behaviour change); the knob exists for an exempted
    // platform, where the slab is what stops us obscuring the desktop at all.
    //
    // The fraction is deliberately screen-relative and NOT derived from the
    // rendered tiger. Sizing the window to the silhouette would be the ideal
    // shape, but the canvas IS the window under Architecture A: shrink the
    // window to the tiger, the tiger re-fits into the smaller canvas, and the
    // pair collapses over a few frames — the same feedback loop #64 had to fix
    // in the zone rect. A screen-relative slab has no such loop.
    private fun applySlabWidth() {
        val pct = try {
            nativeGetSlabPercent()
        } catch (_: Throwable) {
            100
        }
        val lp = window.attributes
        if (pct >= 100 || pct <= 0) {
            if (lp.width == WindowManager.LayoutParams.MATCH_PARENT) return
            lp.width = WindowManager.LayoutParams.MATCH_PARENT
            lp.height = WindowManager.LayoutParams.MATCH_PARENT
            window.attributes = lp
            return
        }
        @Suppress("DEPRECATION")
        val display = windowManager.defaultDisplay
        val real = Point()
        @Suppress("DEPRECATION")
        display.getRealSize(real)
        val want = (maxOf(real.x, real.y) * pct) / 100
        if (lp.width == want && lp.gravity == Gravity.CENTER) return
        lp.width = want
        lp.height = WindowManager.LayoutParams.MATCH_PARENT
        lp.gravity = Gravity.CENTER
        window.attributes = lp
        // A touch-MODAL window's touchable region is the WHOLE DISPLAY however
        // small its frame is: WindowState.getSurfaceTouchableRegion widens it to
        // the display bounds unless FLAG_NOT_TOUCH_MODAL or FLAG_NOT_FOCUSABLE is
        // set. Measured: with the slab applied but the window still modal the
        // frame read [0,576][1600,1984] while the touchable region stayed
        // [0,0][1600,2560] and the avatar kept eating every tap. Only set once we
        // are actually slabbed — at full width it would be a no-op that still
        // changes outside-touch semantics. (Deprecated in S; still honoured, and
        // it is exactly the behaviour named.)
        @Suppress("DEPRECATION")
        window.addFlags(WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL)
        android.util.Log.i(TAG, "#66 slab: window width $want px ($pct% of ${maxOf(real.x, real.y)})")
    }

    private fun installTouchRegionListener() {
        if (insetsProxy != null) return
        try {
            val infoCls = Class.forName("android.view.ViewTreeObserver\$InternalInsetsInfo")
            setTouchableInsetsM =
                infoCls.getMethod("setTouchableInsets", Int::class.javaPrimitiveType)
            touchableRegionF = infoCls.getField("touchableRegion")
            touchableInsetsRegionConst = infoCls.getField("TOUCHABLE_INSETS_REGION").getInt(null)

            val lCls =
                Class.forName("android.view.ViewTreeObserver\$OnComputeInternalInsetsListener")
            insetsListenerCls = lCls
            // equals/hashCode must be answered by hand: the observer keeps
            // listeners in a list and unregistering compares them, and a proxy
            // that returns null for hashCode() NPEs on unboxing.
            val handler = java.lang.reflect.InvocationHandler { proxy, method, args ->
                when (method.name) {
                    "onComputeInternalInsets" -> {
                        if (args != null && args.isNotEmpty()) applyTouchableRegion(args[0])
                        null
                    }
                    "hashCode" -> System.identityHashCode(proxy)
                    "equals" -> args != null && args.isNotEmpty() && args[0] === proxy
                    "toString" -> "AvatarTouchRegionListener"
                    else -> null
                }
            }
            val proxy = java.lang.reflect.Proxy.newProxyInstance(
                lCls.classLoader, arrayOf(lCls), handler,
            )
            val vto = window.decorView.viewTreeObserver
            vto.javaClass.getMethod("addOnComputeInternalInsetsListener", lCls).invoke(vto, proxy)
            insetsProxy = proxy
            android.util.Log.i(TAG, "#66 punch-through armed (TOUCHABLE_INSETS_REGION)")
        } catch (t: Throwable) {
            android.util.Log.w(
                TAG,
                "#66 punch-through unavailable — the whole window stays touchable",
                t,
            )
        }
    }

    private fun removeTouchRegionListener() {
        val proxy = insetsProxy ?: return
        insetsProxy = null
        try {
            val vto = window.decorView.viewTreeObserver
            vto.javaClass
                .getMethod("removeOnComputeInternalInsetsListener", insetsListenerCls)
                .invoke(vto, proxy)
        } catch (_: Throwable) {
        }
    }

    // Called on the UI thread from inside performTraversals.
    private fun applyTouchableRegion(info: Any) {
        try {
            setTouchableInsetsM?.invoke(info, touchableInsetsRegionConst)
            val region = touchableRegionF?.get(info) as? Region ?: return
            region.setEmpty()
            val n = appliedCount
            if (n < 0) {
                // Not measured yet — stay fully touchable so the demo is never
                // dead to input during warmup (same contract as native sil_hit).
                val v = window?.decorView
                if (v != null && v.width > 0 && v.height > 0) region.set(0, 0, v.width, v.height)
                return
            }
            for (i in 0 until n) {
                val o = i * 4
                region.op(
                    appliedRects[o],
                    appliedRects[o + 1],
                    appliedRects[o] + appliedRects[o + 2],
                    appliedRects[o + 1] + appliedRects[o + 3],
                    Region.Op.UNION,
                )
            }
        } catch (_: Throwable) {
        }
    }

    private fun sampleTouchRegion() {
        if (overlayActive) return  // #1110: no Activity window to shape
        if (insetsProxy == null) return
        val n = try {
            nativeGetTouchRegion(scratchRects)
        } catch (_: Throwable) {
            return // native lib not bound yet
        }
        var dirty = n != appliedCount
        if (!dirty && n > 0) {
            for (i in 0 until n * 4) {
                if (kotlin.math.abs(scratchRects[i] - appliedRects[i]) > REGION_EPS_PX) {
                    dirty = true
                    break
                }
            }
        }
        if (!dirty) return
        if (n > 0) System.arraycopy(scratchRects, 0, appliedRects, 0, n * 4)
        appliedCount = n
        // Only a traversal re-runs onComputeInternalInsets, and we never draw
        // through the View system, so ask for one explicitly.
        window.decorView.requestLayout()
    }

    private val displayListener = object : DisplayManager.DisplayListener {
        override fun onDisplayChanged(displayId: Int) {
            pushRotation()
            // Fires even while the Activity is stopped (overlay mode), which
            // onConfigurationChanged does not.
            resizeOverlay()
        }
        override fun onDisplayAdded(displayId: Int) {}
        override fun onDisplayRemoved(displayId: Int) {}
    }

    private fun pushRotation() {
        @Suppress("DEPRECATION")
        val rotation = windowManager.defaultDisplay.rotation // Surface.ROTATION_*
        try {
            nativeSetRotation(rotation)
        } catch (_: Throwable) {
            // Native lib not bound yet — a later display/config change retries.
        }
    }

    // Wake the runtime package before xrCreateInstance. After a force-stop /
    // fresh install the runtime is in Android's "stopped" state, so the loader's
    // broker lookup excludes it → RUNTIME_UNAVAILABLE on a cold tap. An explicit
    // intent with FLAG_INCLUDE_STOPPED_PACKAGES clears the stopped flag so the
    // broker becomes discoverable. (Real apps assume the runtime already ran.)
    private fun wakeRuntime() {
        val pkg = installedRuntime ?: return
        try {
            val intent = Intent("org.khronos.openxr.OpenXRRuntimeService").apply {
                `package` = pkg
                addFlags(Intent.FLAG_INCLUDE_STOPPED_PACKAGES)
            }
            startService(intent)
        } catch (_: Throwable) {
            // Best-effort; the native side retries xrCreateInstance.
        }
    }

    // ──────────────────────────────────────────────── click-through overlay mode
    //
    // runtime#1110, and the answer to the three walls job 5 documents. Those
    // walls were all measured against an ACTIVITY window. Re-measured against a
    // tight, TOUCHABLE TYPE_APPLICATION_OVERLAY with no Activity of ours in the
    // foreground task, on the same stock Android 13 ROM, click-through simply
    // works — with no platform grant, no allowlist and no OEM change. Each
    // property clears exactly one wall:
    //
    //   * TIGHT frame — untrusted-touch occlusion is evaluated AT THE TOUCH POINT
    //     against the FRAMES of the windows above, so a window that does not
    //     contain the tap contributes nothing. The old ghost came from being
    //     full-screen, not from being an overlay. (Wall 2.)
    //   * TOUCHABLE (no FLAG_NOT_TOUCHABLE) — inside its own frame the overlay is
    //     the *touched* window, so the opacity policy is never consulted for it.
    //     This is also what stops the PLATFORM clamping us: requesting alpha 1.0
    //     on a FLAG_NOT_TOUCHABLE application overlay yields alpha=0.80 in
    //     `dumpsys input` and a visible 20% blend on screencap; touchable keeps
    //     1.00 exactly. The 0.80 was never only self-imposed. (Wall 2 again.)
    //   * No Activity of ours in the FOREGROUND TASK — moveTaskToBack(true) is
    //     enough: a backgrounded Activity's ActivityRecordInputSink goes
    //     NOT_VISIBLE|NOT_TOUCHABLE, i.e. inert, so we keep the Activity and with
    //     it android_main, the asset manager and this JNI bridge. Anything that
    //     re-foregrounds us (Recents, a re-launch) silently re-arms it. (Wall 3.)
    //
    // Wall 1 — the per-region touchable API — is untouched: re-measured for
    // #1110, InternalInsetsInfo.touchableRegion is blocklisted for OVERLAY
    // windows exactly as for Activity windows (the blocklist is per-API, not
    // per-window-type). So click-through here is per-FRAME, not per-pixel: a tap
    // on a transparent corner inside Leo's box still hits Leo. A tight frame
    // shrinks that dead area from the whole panel to his bounding box, which is
    // why job 5's machinery stays — it is still the fix for the residue.
    //
    // Cost: SYSTEM_ALERT_WINDOW (a user-granted special app access) and a
    // foreground service. Without the grant we fall back to the Activity-window
    // topology above, which still renders and weaves — it just swallows taps.
    private var overlayRoot: FrameLayout? = null
    private var overlaySurfaceView: SurfaceView? = null
    private var overlayActive = false
    private var quitting = false

    // Leo's window aspect (w:h). The renderer auto-fits the character to whatever
    // canvas it gets, so this is purely how much desktop we leave clickable.
    private val overlayAspect = 0.75f

    // #1110 rotation fix: an application context never receives configuration
    // updates, so a window added through its WindowManager stays anchored to the
    // boot-time display rotation — after a rotate, SF composites the stale-framed
    // portrait buffer through a transform every frame (whole-window flicker). A
    // window context (API 30+) tracks the display configuration, so WM re-lays
    // the overlay out on rotation. Pre-30 keeps the old behaviour.
    private var overlayWindowContextCache: Context? = null
    private val overlayWindowContext: Context
        get() {
            overlayWindowContextCache?.let { return it }
            val ctx = if (Build.VERSION.SDK_INT >= 30) {
                val dm = getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
                val display = dm.getDisplay(android.view.Display.DEFAULT_DISPLAY)
                applicationContext.createDisplayContext(display)
                    .createWindowContext(WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY, null)
            } else {
                applicationContext
            }
            overlayWindowContextCache = ctx
            return ctx
        }

    private val overlayWindowManager: WindowManager
        get() = overlayWindowContext.getSystemService(Context.WINDOW_SERVICE) as WindowManager

    /**
     * Overlay size in px: a 3:4 box whose HEIGHT is the panel's short edge, scaled
     * by debug.dxr.avatar.overlay.slab (%). (Distinct from job 5's
     * debug.dxr.avatar.slab, which shrinks the ACTIVITY window on the fallback
     * path.) Keying off the short edge rather than the current orientation keeps
     * Leo the same physical size when the pad is rotated, and leaves the flanks
     * live in landscape / the whole upper screen live in portrait.
     *
     * TRAP: read the panel from the OVERLAY's display, not the Activity's. In
     * overlay mode the Activity is stopped, and a stopped Activity's
     * WindowManager keeps handing back the metrics of the orientation it was last
     * resumed in — a rotation would resize the overlay against the OLD panel,
     * silently.
     */
    private fun overlayExtent(): Point {
        val real = Point()
        val display = overlayRoot?.display ?: window?.decorView?.display
        @Suppress("DEPRECATION")
        if (display != null) display.getRealSize(real) else windowManager.defaultDisplay.getRealSize(real)
        if (real.x <= 0 || real.y <= 0) return Point(1, 1)
        val pct = nativeGetIntProp("debug.dxr.avatar.overlay.slab", 100).coerceIn(10, 100)
        var h = (minOf(real.x, real.y) * (pct / 100f)).toInt()
        var w = (h * overlayAspect).toInt()
        if (w > real.x) { w = real.x; h = (w / overlayAspect).toInt() }
        if (h > real.y) { h = real.y; w = (h * overlayAspect).toInt() }
        return Point(w, h)
    }

    private fun overlayLayoutParams(): WindowManager.LayoutParams {
        val e = overlayExtent()
        val type = if (Build.VERSION.SDK_INT >= 26) {
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
        } else {
            @Suppress("DEPRECATION")
            WindowManager.LayoutParams.TYPE_PHONE
        }
        val lp = WindowManager.LayoutParams(
            e.x, e.y, type,
            // NOT_FOCUSABLE: we never want the IME or the back key.
            // NOT_TOUCH_MODAL: without it a window's touchable region is the WHOLE
            //   display however small its frame is — the same trap job 5 hit, and
            //   the tight frame would buy nothing.
            // LAYOUT_NO_LIMITS: keeps the frame in raw panel coordinates, which is
            //   what the weave phase and the Kooima canvas are anchored to.
            // Deliberately NOT FLAG_NOT_TOUCHABLE — see the note above.
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or
                WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
            PixelFormat.TRANSLUCENT,
        )
        lp.gravity = Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL
        lp.alpha = 1.0f
        return lp
    }

    private val overlayHolderCallback = object : SurfaceHolder.Callback {
        override fun surfaceCreated(holder: SurfaceHolder) {
            nativeSetOverlaySurface(holder.surface)
            // Only NOW is it safe to leave the foreground task: the renderer has a
            // window and our sink can go inert.
            leaveForegroundTask()
        }

        override fun surfaceChanged(holder: SurfaceHolder, fmt: Int, w: Int, h: Int) {
            // Re-publish: a resize (rotation) gives us a brand new buffer queue.
            nativeSetOverlaySurface(holder.surface)
            lastRect = intArrayOf(Int.MIN_VALUE, Int.MIN_VALUE, -1, -1, -1, -1, -1)
            sampleWindowRect()
        }

        override fun surfaceDestroyed(holder: SurfaceHolder) {
            nativeSetOverlaySurface(null)
        }
    }

    /** true if the overlay came up; false means stay on the Activity-window path. */
    private fun startOverlayMode(): Boolean {
        if (overlayActive) return true
        if (nativeGetIntProp("debug.dxr.avatar.overlay", 1) == 0) return false
        if (!Settings.canDrawOverlays(this)) return false

        val sv = SurfaceView(overlayWindowContext).apply {
            // Above the (empty) host window, and alpha-blended against whatever is
            // behind the overlay — this is what lets the desktop show through
            // around Leo at full opacity.
            setZOrderOnTop(true)
            holder.setFormat(PixelFormat.TRANSLUCENT)
            holder.addCallback(overlayHolderCallback)
        }
        // A SurfaceView needs a real ViewParent: added bare it trips
        // requestTransparentRegion on a null parent (SurfaceView.java:294).
        val root = FrameLayout(overlayWindowContext).apply {
            addView(
                sv,
                FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT,
                    FrameLayout.LayoutParams.MATCH_PARENT,
                ),
            )
            setOnTouchListener { _, ev -> onOverlayTouch(ev); true }
        }
        return try {
            overlayWindowManager.addView(root, overlayLayoutParams())
            overlayRoot = root
            overlaySurfaceView = sv
            overlayActive = true
            startForegroundService(Intent(this, AvatarOverlayService::class.java))
            true
        } catch (t: Throwable) {
            android.util.Log.w(TAG, "overlay addView failed; staying on the Activity window", t)
            false
        }
    }

    private fun onOverlayTouch(event: MotionEvent) {
        gestureDetector.onTouchEvent(event)
        val n = event.pointerCount
        val x1 = if (n > 1) event.getX(1) else 0f
        val y1 = if (n > 1) event.getY(1) else 0f
        try {
            // Overlay-local px ARE canvas px: the overlay IS the canvas.
            nativeOnTouch(event.actionMasked, n, event.getX(0), event.getY(0), x1, y1)
        } catch (_: Throwable) {
        }
    }

    private fun leaveForegroundTask() {
        if (!overlayActive || quitting) return
        Handler(Looper.getMainLooper()).post {
            if (!quitting) {
                val ok = moveTaskToBack(true)
                android.util.Log.i(TAG, "#1110 overlay mode: moveTaskToBack -> $ok")
            }
        }
    }

    private fun resizeOverlay() {
        val root = overlayRoot ?: return
        try {
            overlayWindowManager.updateViewLayout(root, overlayLayoutParams())
        } catch (t: Throwable) {
            android.util.Log.w(TAG, "overlay resize failed", t)
        }
    }

    private fun stopOverlayMode() {
        overlayRoot?.let {
            try { overlayWindowManager.removeView(it) } catch (_: Throwable) {}
        }
        overlayRoot = null
        overlaySurfaceView = null
        overlayActive = false
        try { stopService(Intent(this, AvatarOverlayService::class.java)) } catch (_: Throwable) {}
    }

    /**
     * Re-launching from the launcher (or tapping the notification) while the
     * overlay is up means quit — with no foreground Activity and no window chrome
     * there is nothing else to close, and bringing the Activity back to the front
     * would re-arm the input sink and kill click-through.
     */
    override fun onNewIntent(intent: Intent?) {
        super.onNewIntent(intent)
        if (overlayActive) {
            quitting = true
            stopOverlayMode()
            finishAndRemoveTask()
        }
    }

    // SYSTEM_ALERT_WINDOW is a special app access, not a runtime permission: only
    // the user can grant it, in Settings. Ask once; keep running either way.
    private fun requestOverlayPermissionOnce() {
        if (Settings.canDrawOverlays(this)) return
        try {
            AlertDialog.Builder(this)
                .setTitle("Let Leo float over your desktop")
                .setMessage(
                    "Leo needs the \"display over other apps\" permission to sit on " +
                        "your desktop and let you tap the icons behind him.\n\n" +
                        "Without it he still appears, but he swallows every tap.",
                )
                .setPositiveButton("Grant") { _, _ ->
                    try {
                        startActivity(
                            Intent(
                                Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                                Uri.parse("package:$packageName"),
                            ),
                        )
                    } catch (_: Throwable) {
                    }
                }
                .setNegativeButton("Not now", null)
                .show()
        } catch (_: Throwable) {
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        wakeRuntime()
        super.onCreate(savedInstanceState)
        // Architecture A: OUR window is the one the runtime weaves into, so it is
        // the one that has to carry alpha. Theme.Avatar.Transparent alone is not
        // enough — a NativeActivity's Surface comes up in an opaque format and
        // SurfaceFlinger then ignores the alpha the compositor writes. Setting
        // TRANSLUCENT here is what actually lets the launcher show through.
        window.setFormat(PixelFormat.TRANSLUCENT)
        // runtime#1110: prefer the click-through overlay topology. Only if it is
        // unavailable (no SAW grant) do we stay on this Activity's window and arm
        // job 5's inert machinery for it.
        if (!startOverlayMode()) {
            requestOverlayPermissionOnce()
            applySlabWidth()
            // #66: arm the punch-through before the first frame, so the window is
            // never briefly a full-screen input sink once the tiger is up.
            installTouchRegionListener()
        }
        requestCameraOnce()
        pushRotation()
        (getSystemService(Context.DISPLAY_SERVICE) as DisplayManager)
            .registerDisplayListener(displayListener, null)
        watchForRuntimeUnavailable()
        showControlsHint()
    }

    // Brief on-screen legend of the touch controls (gesture-driven, no on-screen
    // buttons). A Toast sits above our window, so no Vulkan HUD needed.
    private fun showControlsHint() {
        Handler(Looper.getMainLooper()).postDelayed({
            if (!isFinishing) {
                Toast.makeText(
                    this,
                    "Drag up/down: depth · Drag left/right: move · Pinch: zoom · Double-tap: recenter",
                    Toast.LENGTH_LONG,
                ).show()
            }
        }, 2500)
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        pushRotation()
        // The screen's long edge swapped; the slab is a fraction of it.
        if (!overlayActive) applySlabWidth()
        // The overlay is sized in panel px, so a rotation changes it — and an
        // overlay window gets no layout pass out of the rotation on its own.
        resizeOverlay()
    }

    override fun onResume() {
        super.onResume()
        pushRotation()
        // Granting SAW sends the user to Settings and back — retry here.
        if (!overlayActive && !quitting) startOverlayMode()
        // Recents (or anything else that foregrounds us) would re-arm the
        // ActivityRecordInputSink and silently kill click-through. Step back out.
        if (overlayActive) leaveForegroundTask()
        if (!rectPollRunning) {
            // Forget the last sample so the first frame after a resume always
            // re-pushes: the surface was destroyed and rebuilt underneath us and
            // the runtime has to be told the rect again, even when it is
            // byte-identical to the one before we went away.
            lastRect = intArrayOf(Int.MIN_VALUE, Int.MIN_VALUE, -1, -1, -1, -1, -1)
            // Same reasoning for the touch region: the window was torn down and
            // rebuilt, and a rotation may have moved the tiger, so re-push it
            // even if native reports a byte-identical region.
            appliedCount = Int.MIN_VALUE
            if (!overlayActive) installTouchRegionListener()
            rectPollRunning = true
            Choreographer.getInstance().postFrameCallback(rectCallback)
        }
    }

    override fun onPause() {
        // In overlay mode the Activity is SUPPOSED to be out of the foreground —
        // the overlay window is still up and being rendered into, so the rect poll
        // has to keep running (it is what anchors the weave phase).
        if (!overlayActive) {
            rectPollRunning = false
            Choreographer.getInstance().removeFrameCallback(rectCallback)
        }
        super.onPause()
    }

    override fun onDestroy() {
        stopOverlayMode()
        removeTouchRegionListener()
        try {
            (getSystemService(Context.DISPLAY_SERVICE) as DisplayManager)
                .unregisterDisplayListener(displayListener)
        } catch (_: Throwable) {
        }
        super.onDestroy()
    }
}
