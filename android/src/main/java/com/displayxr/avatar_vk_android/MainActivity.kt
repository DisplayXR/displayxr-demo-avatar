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
import android.hardware.display.DisplayManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Choreographer
import android.view.GestureDetector
import android.view.MotionEvent
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
            Choreographer.getInstance().postFrameCallback(this)
        }
    }

    private fun sampleWindowRect() {
        val view = window?.decorView ?: return
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

    private val displayListener = object : DisplayManager.DisplayListener {
        override fun onDisplayChanged(displayId: Int) = pushRotation()
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

    override fun onCreate(savedInstanceState: Bundle?) {
        wakeRuntime()
        super.onCreate(savedInstanceState)
        // Architecture A: OUR window is the one the runtime weaves into, so it is
        // the one that has to carry alpha. Theme.Avatar.Transparent alone is not
        // enough — a NativeActivity's Surface comes up in an opaque format and
        // SurfaceFlinger then ignores the alpha the compositor writes. Setting
        // TRANSLUCENT here is what actually lets the launcher show through.
        window.setFormat(PixelFormat.TRANSLUCENT)
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
    }

    override fun onResume() {
        super.onResume()
        pushRotation()
        if (!rectPollRunning) {
            // Forget the last sample so the first frame after a resume always
            // re-pushes: the surface was destroyed and rebuilt underneath us and
            // the runtime has to be told the rect again, even when it is
            // byte-identical to the one before we went away.
            lastRect = intArrayOf(Int.MIN_VALUE, Int.MIN_VALUE, -1, -1, -1, -1, -1)
            rectPollRunning = true
            Choreographer.getInstance().postFrameCallback(rectCallback)
        }
    }

    override fun onPause() {
        rectPollRunning = false
        Choreographer.getInstance().removeFrameCallback(rectCallback)
        super.onPause()
    }

    override fun onDestroy() {
        try {
            (getSystemService(Context.DISPLAY_SERVICE) as DisplayManager)
                .unregisterDisplayListener(displayListener)
        } catch (_: Throwable) {
        }
        super.onDestroy()
    }
}
