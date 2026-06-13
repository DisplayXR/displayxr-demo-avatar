// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// Thin NativeActivity wrapper for the vendor-neutral model viewer. Three jobs:
//   1. Push the authoritative 4-way display rotation to native on launch and on
//      every rotation (incl. 180° flips, via a DisplayListener) — the renderer
//      can't derive true rotation from its own surface, and
//      Configuration.orientation only distinguishes portrait/landscape.
//   2. Forward touch from the runtime's MonadoView overlay (which covers our
//      window and is the only view that receives touch) to native via
//      dispatchTouchEvent (runtime#499) — a NativeActivity's native input queue
//      is NOT fed by dispatchTouchEvent.
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
import android.graphics.PixelFormat
import android.graphics.drawable.GradientDrawable
import android.hardware.display.DisplayManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.GestureDetector
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.LinearLayout
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

        private const val REQUEST_PICK_MODEL = 1001
    }

    // Implemented in main.cpp. rotation = Surface.ROTATION_0/90/180/270 → 0/1/2/3.
    private external fun nativeSetRotation(rotation: Int)

    // True once xrCreateInstance failed with RUNTIME_UNAVAILABLE.
    private external fun nativeRuntimeUnavailable(): Boolean

    // True once the OpenXR instance is up (runtime reached).
    private external fun nativeXrReady(): Boolean

    // Touch bridge: single-finger drag = orbit; two-finger pinch = zoom
    // (handled native-side). The MonadoView overlay forwards events here.
    private external fun nativeOnTouch(
        action: Int, count: Int, x0: Float, y0: Float, x1: Float, y1: Float,
    )

    // Double-tap resets the viewpoint to the initial framing (instant; no
    // model reload — model switching moves to the planned button UI).
    private external fun nativeResetView()

    // Single tap (not part of a double-tap): shows / interacts with the native
    // window-space button bar. Coords in view pixels.
    private external fun nativeOnTap(x: Float, y: Float)

    // Polled: true once when the native Open button was tapped → launch the
    // SAF document picker (native code can't start an Intent itself).
    private external fun nativeConsumeOpenRequest(): Boolean

    // Hand a picked model (already copied into app-private storage) to native.
    private external fun nativeOpenModelPath(path: String)

    // ── Widget button bar (default UI while runtime#506 is open) ────────────
    // True when debug.dxr.mv.ws_ui=1 selected the native window-space bar
    // instead (the #506 test client) — taps are then forwarded to nativeOnTap.
    private external fun nativeUseWindowSpaceUi(): Boolean

    // Mode button → next display rendering mode (serviced on android_main).
    private external fun nativeCycleMode()

    // Animation button → next clip / play-pause toggle.
    private external fun nativeAnimAction()

    // "Mode: <name>" for the Mode button.
    private external fun nativeGetModeLabel(): String

    // Clip name / "Paused"; empty = model has no animations (hide the button).
    private external fun nativeGetAnimLabel(): String

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

                override fun onSingleTapConfirmed(e: MotionEvent): Boolean {
                    try {
                        if (nativeUseWindowSpaceUi()) {
                            nativeOnTap(e.x, e.y)  // #506 test path
                        } else {
                            showButtonBar()
                        }
                    } catch (_: Throwable) {
                    }
                    return true
                }
            },
        )
    }

    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        gestureDetector.onTouchEvent(event) // tap → menu, double-tap → re-frame
        // Any touch on the scene re-arms the bar's 5 s idle timer (touches on
        // the bar's buttons land in its own window and re-arm via onClick).
        if (barView?.visibility == View.VISIBLE) armBarHideTimer()
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
        watchForOpenRequest()
        showControlsHint()
    }

    // ── Widget button bar ([Open] [Mode: <name>] [<anim>]) ──────────────────
    // The runtime's MonadoView weave surface is its own WindowManager window
    // ABOVE the activity, so plain content views would be hidden under it. The
    // bar is therefore added as ANOTHER application window — lazily, on first
    // show, which is always after MonadoView attached (taps arrive through it)
    // → later windows stack on top. The window is a top strip (WRAP_CONTENT
    // height), so scene touches outside it still reach MonadoView.
    private var barView: LinearLayout? = null
    private var modeButton: Button? = null
    private var animButton: Button? = null
    private val barHandler = Handler(Looper.getMainLooper())
    private val hideBarRunnable = Runnable { hideButtonBar() }

    private fun pillBackground(): GradientDrawable {
        val density = resources.displayMetrics.density
        return GradientDrawable().apply {
            cornerRadius = 24f * density
            setColor(0xD11A1C21.toInt())          // dark translucent fill
            setStroke((1.5f * density).toInt(), 0x8C8C94A0.toInt())  // light rim
        }
    }

    private fun makePill(label: String, onClick: () -> Unit): Button {
        val density = resources.displayMetrics.density
        return Button(this).apply {
            text = label
            isAllCaps = false
            setTextColor(0xFFFFFFFF.toInt())
            textSize = 16f
            background = pillBackground()
            stateListAnimator = null
            setPadding((20 * density).toInt(), (8 * density).toInt(),
                (20 * density).toInt(), (8 * density).toInt())
            minimumWidth = 0
            minimumHeight = 0
            setOnClickListener {
                onClick()
                armBarHideTimer()
            }
        }
    }

    private fun ensureBar() {
        if (barView != null) return
        val density = resources.displayMetrics.density
        val bar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding((14 * density).toInt(), (10 * density).toInt(),
                (14 * density).toInt(), (10 * density).toInt())
        }
        val pillLp = LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT,
        ).apply { marginEnd = (10 * density).toInt() }
        bar.addView(makePill("Open") { launchModelPicker() }, pillLp)
        modeButton = makePill("Mode") {
            try { nativeCycleMode() } catch (_: Throwable) {}
            scheduleLabelRefresh()
        }
        bar.addView(modeButton, pillLp)
        // Spacer pushes the animation pill to the right edge (Windows layout).
        bar.addView(View(this), LinearLayout.LayoutParams(0, 1, 1f))
        animButton = makePill("Anim") {
            try { nativeAnimAction() } catch (_: Throwable) {}
            scheduleLabelRefresh()
        }
        bar.addView(animButton, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT,
        ))
        val wlp = WindowManager.LayoutParams().apply {
            width = WindowManager.LayoutParams.MATCH_PARENT
            height = WindowManager.LayoutParams.WRAP_CONTENT
            gravity = Gravity.TOP
            type = WindowManager.LayoutParams.TYPE_APPLICATION
            flags = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
            format = PixelFormat.TRANSLUCENT
        }
        windowManager.addView(bar, wlp)
        barView = bar
    }

    private fun showButtonBar() {
        try {
            ensureBar()
            refreshBarLabels()
            barView?.apply {
                animate().cancel()
                alpha = 1f
                visibility = View.VISIBLE
            }
            armBarHideTimer()
        } catch (t: Throwable) {
            android.util.Log.e("avatar", "button bar show failed", t)
        }
    }

    private fun armBarHideTimer() {
        barHandler.removeCallbacks(hideBarRunnable)
        barHandler.postDelayed(hideBarRunnable, 5000)
    }

    private fun hideButtonBar() {
        barView?.animate()?.alpha(0f)?.setDuration(400)?.withEndAction {
            barView?.visibility = View.GONE
        }?.start()
    }

    private fun refreshBarLabels() {
        try {
            modeButton?.text = nativeGetModeLabel()
            val anim = nativeGetAnimLabel()
            animButton?.visibility = if (anim.isEmpty()) View.GONE else View.VISIBLE
            if (anim.isNotEmpty()) animButton?.text = anim
        } catch (_: Throwable) {
        }
    }

    // Mode/anim state changes land asynchronously on the XR thread — refresh
    // shortly after a click (twice, to catch the mode-changed event).
    private fun scheduleLabelRefresh() {
        barHandler.postDelayed({ refreshBarLabels() }, 350)
        barHandler.postDelayed({ refreshBarLabels() }, 1000)
    }

    // ── Open-model flow (native button bar → SAF picker → native load) ──────
    // Native can't start an Intent, so a light poll consumes its open request
    // (same polling pattern as watchForRuntimeUnavailable).
    private fun watchForOpenRequest() {
        val handler = Handler(Looper.getMainLooper())
        handler.postDelayed(
            object : Runnable {
                override fun run() {
                    if (isFinishing) return
                    val requested = try { nativeConsumeOpenRequest() } catch (_: Throwable) { false }
                    if (requested) launchModelPicker()
                    handler.postDelayed(this, 250)
                }
            },
            2000,
        )
    }

    private fun launchModelPicker() {
        try {
            val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                // .glb/.gltf usually surface as octet-stream; let everything
                // through and let the native loader reject non-glTF.
                type = "*/*"
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
            startActivityForResult(intent, REQUEST_PICK_MODEL)
        } catch (t: Throwable) {
            android.util.Log.e("avatar", "model picker launch failed", t)
        }
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        @Suppress("DEPRECATION")
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQUEST_PICK_MODEL || resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        // Copy to app-private storage off the UI thread (the glTF loader needs
        // a filesystem path; SAF only hands out a content URI).
        Thread {
            try {
                val name = (uri.lastPathSegment ?: "picked.glb").substringAfterLast('/')
                val ext = if (name.endsWith(".gltf", true)) ".gltf" else ".glb"
                val dst = java.io.File(filesDir, "picked$ext")
                contentResolver.openInputStream(uri)?.use { input ->
                    dst.outputStream().use { output -> input.copyTo(output) }
                } ?: return@Thread
                android.util.Log.i("avatar", "picked model staged to ${dst.absolutePath}")
                nativeOpenModelPath(dst.absolutePath)
            } catch (t: Throwable) {
                android.util.Log.e("avatar", "picked model staging failed", t)
            }
        }.start()
    }

    // Brief on-screen legend of the touch controls (gesture-driven, no on-screen
    // buttons). A Toast sits above the weave overlay, so no Vulkan HUD needed.
    private fun showControlsHint() {
        Handler(Looper.getMainLooper()).postDelayed({
            if (!isFinishing) {
                Toast.makeText(
                    this,
                    "Drag: rotate · Pinch: zoom · Tap: menu · Double-tap: re-frame",
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
        barHandler.removeCallbacks(hideBarRunnable)
        barView?.let {
            try {
                windowManager.removeViewImmediate(it)
            } catch (_: Throwable) {
            }
        }
        barView = null
        super.onDestroy()
    }
}
