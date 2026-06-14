// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// Thin NativeActivity wrapper for the avatar demo. Three jobs:
//   1. Push the authoritative 4-way display rotation to native on launch and on
//      every rotation (incl. 180° flips, via a DisplayListener) — the renderer
//      can't derive true rotation from its own surface, and
//      Configuration.orientation only distinguishes portrait/landscape.
//   2. Forward touch from the runtime's MonadoView overlay (which covers our
//      window and is the only view that receives touch) to native via
//      dispatchTouchEvent (runtime#499) — a NativeActivity's native input queue
//      is NOT fed by dispatchTouchEvent. One finger = camera controls (depth
//      dolly + lateral strafe), two fingers = pinch-zoom, double-tap = recenter.
//   3. Wake the DisplayXR runtime out of Android's "stopped" state before
//      xrCreateInstance and, if it can't be reached, prompt the user.
//
// Vendor-neutral + out-of-process (ADR-025): no CNSDK, no CAMERA — the OOP
// runtime service owns eye tracking. Binds to whichever runtime flavor is
// installed (out_of_process preferred, in_process dev fallback).

package com.displayxr.avatar_vk_android

import android.app.AlertDialog
import android.app.NativeActivity
import android.content.Context
import android.content.Intent
import android.content.res.Configuration
import android.hardware.display.DisplayManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
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

    // True when debug.dxr.overlay=1: the runtime weaves the tiger into a
    // service-owned overlay over the live launcher, so this Activity should step
    // to the background (keep-alive foreground service keeps the render loop off
    // the freezer) instead of sitting on top and pausing the launcher.
    private external fun nativeOverlayMode(): Boolean

    private var overlayEntered = false

    // Touch bridge: one finger = camera controls (depth dolly + lateral strafe),
    // two-finger pinch = zoom (all handled native-side). The MonadoView overlay
    // forwards events here.
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
                    if (ready) {
                        maybeEnterOverlay()
                        return
                    }
                    if (tries++ < 15) handler.postDelayed(this, 1000)
                }
            },
            2000,
        )
    }

    // #558 overlay mode: once the session is up, start a keep-alive foreground
    // service (exempts the process from the freezer) and step to the background
    // so the launcher resumes + stays interactive while the runtime weaves the
    // tiger into its service overlay on top. One-shot.
    private fun maybeEnterOverlay() {
        if (overlayEntered) return
        val overlay = try { nativeOverlayMode() } catch (_: Throwable) { false }
        if (!overlay) return
        overlayEntered = true
        try {
            OverlayKeepAliveService.start(this)
        } catch (t: Throwable) {
            android.util.Log.e("avatar", "keep-alive service start failed", t)
        }
        // Brief beat so the runtime's overlay surface is live before we vacate the
        // foreground; the native render loop keeps drawing with no window.
        Handler(Looper.getMainLooper()).postDelayed({
            if (!isFinishing) {
                android.util.Log.i("avatar", "overlay mode: moveTaskToBack — launcher resumes")
                moveTaskToBack(true)
            }
        }, 600)
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
        pushRotation()
        (getSystemService(Context.DISPLAY_SERVICE) as DisplayManager)
            .registerDisplayListener(displayListener, null)
        watchForRuntimeUnavailable()
        showControlsHint()
    }

    // Brief on-screen legend of the touch controls (gesture-driven, no on-screen
    // buttons). A Toast sits above the weave overlay, so no Vulkan HUD needed.
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
    }

    override fun onDestroy() {
        try {
            (getSystemService(Context.DISPLAY_SERVICE) as DisplayManager)
                .unregisterDisplayListener(displayListener)
        } catch (_: Throwable) {
        }
        try {
            OverlayKeepAliveService.stop(this)
        } catch (_: Throwable) {
        }
        super.onDestroy()
    }
}
