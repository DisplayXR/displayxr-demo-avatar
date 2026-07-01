// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// #558 overlay mode: a foreground service with two jobs while the avatar's
// Activity is backgrounded (MainActivity.moveTaskToBack):
//
//   1. Keep the avatar process off the Android background freezer so the OpenXR
//      render loop (the NativeActivity's android_main thread) keeps weaving the
//      tiger into the runtime's service-owned TYPE_APPLICATION_OVERLAY over the
//      LIVE launcher.
//   2. Host a small TYPE_APPLICATION_OVERLAY *input* window laid out to the
//      tiger's bounding box (#13). The runtime's visual overlay is
//      FLAG_NOT_TOUCHABLE (full passthrough so the launcher stays usable), so the
//      tiger itself would get no touch. This window is a plain transparent View
//      (NOT a SurfaceView → no anti-tapjacking alpha clamp), touchable, that
//      follows the tiger and forwards gestures to the same native camera controls
//      the foreground path uses. Everything outside the bbox isn't covered → falls
//      through to the launcher.
//
// The Activity still owns the native lib + render thread; this service only holds
// the process foreground and the input window. Gated on debug.dxr.overlay
// (MainActivity starts it only then).

package com.displayxr.avatar_vk_android

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.PixelFormat
import android.os.Build
import android.os.IBinder
import android.provider.Settings
import android.view.Choreographer
import android.view.GestureDetector
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager

class OverlayKeepAliveService : Service() {

    private var windowManager: WindowManager? = null
    private var inputView: View? = null
    private var lp: WindowManager.LayoutParams? = null
    private val bounds = IntArray(4) // reused [x,y,w,h] from native, avoids per-frame alloc
    private var frameCallback: Choreographer.FrameCallback? = null
    private lateinit var gestureDetector: GestureDetector
    private var gestureOnTiger = false // did the in-progress gesture start on the tiger?

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val channelId = "dxr_avatar_overlay"
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            if (nm.getNotificationChannel(channelId) == null) {
                nm.createNotificationChannel(
                    NotificationChannel(
                        channelId, "DisplayXR Avatar", NotificationManager.IMPORTANCE_LOW,
                    ),
                )
            }
        }
        val notif: Notification =
            Notification.Builder(this, channelId)
                .setContentTitle("DisplayXR Avatar")
                .setContentText("Leo is floating on your desktop")
                .setSmallIcon(android.R.drawable.ic_menu_view)
                .setOngoing(true)
                .build()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(NOTIF_ID, notif, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
        } else {
            startForeground(NOTIF_ID, notif)
        }
        addInputOverlay()
        return START_STICKY
    }

    // Add the tiger-bbox input window + start the follower. Needs the
    // draw-overlays grant (the avatar requests it on entering overlay mode; the
    // test recipe also pre-grants via appops). No-op if already added or ungranted.
    private fun addInputOverlay() {
        if (inputView != null) return
        if (!Settings.canDrawOverlays(this)) {
            android.util.Log.w("avatar", "tiger input overlay: SYSTEM_ALERT_WINDOW not granted — skip")
            return
        }
        gestureDetector =
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

        val wm = getSystemService(Context.WINDOW_SERVICE) as WindowManager
        val params =
            WindowManager.LayoutParams(
                0, 0, // start collapsed; the follower sizes it to the tiger bbox
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                // Not focusable (don't steal launcher focus) + no layout limits so it
                // can sit anywhere on screen. Touchable (NO FLAG_NOT_TOUCHABLE) so it
                // receives the tiger's gestures; a plain View has no 0.80 alpha clamp.
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                    WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                PixelFormat.TRANSLUCENT,
            ).apply {
                gravity = Gravity.TOP or Gravity.START
                x = 0
                y = 0
            }

        val view =
            View(this).apply {
                setOnTouchListener { _, event ->
                    // Use RAW (display-absolute) coords, NOT window-local getX/getY:
                    // the input window is repositioned every frame to follow the
                    // tiger, so window-local coords jump under a stationary finger
                    // (the foreground path didn't, hence it was smooth). getRawX/Y are
                    // measured against the display → stable through the window move,
                    // and they're already in canvas px (the tiger zone is in display
                    // coords). The window that caught the DOWN keeps the gesture even
                    // if it moves out from under the finger. (#13 jump fix)
                    val n = event.pointerCount
                    val rx0 = event.getRawX(0)
                    val ry0 = event.getRawY(0)
                    // Silhouette gate: decide on the FIRST finger-down whether this
                    // gesture is on the tiger. If it lands on a non-tiger corner of the
                    // bbox, return false so we don't consume it — the tap then falls to
                    // whatever is under the bbox (launcher, another app, a permission
                    // dialog) instead of being eaten. A gesture that began on the tiger
                    // keeps control even if the finger slides off. (#13)
                    if (event.actionMasked == MotionEvent.ACTION_DOWN) {
                        gestureOnTiger = try { nativeHitTiger(rx0, ry0) } catch (_: Throwable) { true }
                    }
                    if (!gestureOnTiger) return@setOnTouchListener false
                    gestureDetector.onTouchEvent(event) // double-tap → recenter
                    val x1 = if (n > 1) event.getRawX(1) else 0f
                    val y1 = if (n > 1) event.getRawY(1) else 0f
                    try {
                        nativeOnTouch(event.actionMasked, n, rx0, ry0, x1, y1)
                    } catch (_: Throwable) {
                    }
                    true
                }
            }

        try {
            wm.addView(view, params)
        } catch (t: Throwable) {
            android.util.Log.e("avatar", "tiger input overlay addView failed", t)
            return
        }
        windowManager = wm
        inputView = view
        lp = params
        android.util.Log.i("avatar", "tiger input overlay added (#13)")
        startFollower()
    }

    // Each frame, pull the tiger bbox from native and move/resize the input window
    // to it. A View has no BufferQueue, so per-frame updateViewLayout is cheap.
    // When the tiger covers nothing (warmup / off-screen), collapse to 0×0 so the
    // window captures no launcher touches.
    private fun startFollower() {
        val cb =
            object : Choreographer.FrameCallback {
                override fun doFrame(frameTimeNanos: Long) {
                    val wm = windowManager
                    val view = inputView
                    val p = lp
                    if (wm == null || view == null || p == null) return
                    val valid = try { nativeGetTigerBounds(bounds) } catch (_: Throwable) { false }
                    val nx = if (valid) bounds[0] else 0
                    val ny = if (valid) bounds[1] else 0
                    val nw = if (valid) bounds[2] else 0
                    val nh = if (valid) bounds[3] else 0
                    if (p.x != nx || p.y != ny || p.width != nw || p.height != nh) {
                        p.x = nx
                        p.y = ny
                        p.width = nw
                        p.height = nh
                        try {
                            wm.updateViewLayout(view, p)
                        } catch (_: Throwable) {
                        }
                    }
                    Choreographer.getInstance().postFrameCallback(this)
                }
            }
        frameCallback = cb
        Choreographer.getInstance().postFrameCallback(cb)
    }

    // The user dismissed the avatar from recents (swiped the card away). In overlay
    // mode the Activity is already backgrounded and this FGS (START_STICKY) would
    // otherwise keep the process alive — leaving the runtime's service overlay
    // frozen on the launcher. Stop ourselves so the process can be reaped; the
    // OpenXR client connection then drops and the runtime tears down its overlay on
    // the last-client disconnect (#558). Also remove our input window up front.
    override fun onTaskRemoved(rootIntent: Intent?) {
        teardownInputOverlay()
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
        super.onTaskRemoved(rootIntent)
    }

    private fun teardownInputOverlay() {
        frameCallback?.let { Choreographer.getInstance().removeFrameCallback(it) }
        frameCallback = null
        val view = inputView
        val wm = windowManager
        if (view != null && wm != null) {
            try {
                wm.removeView(view)
            } catch (_: Throwable) {
            }
        }
        inputView = null
        windowManager = null
        lp = null
    }

    override fun onDestroy() {
        teardownInputOverlay()
        super.onDestroy()
    }

    // Native bridge (same process / lib as MainActivity). Touch flows in CANVAS px.
    private external fun nativeGetTigerBounds(out: IntArray): Boolean
    private external fun nativeOnTouch(
        action: Int, count: Int, x0: Float, y0: Float, x1: Float, y1: Float,
    )
    private external fun nativeResetView()

    // True if a canvas-px point is on the tiger silhouette (used to gate the gesture
    // so non-tiger taps in the bbox aren't consumed).
    private external fun nativeHitTiger(x: Float, y: Float): Boolean

    companion object {
        private const val NOTIF_ID = 0x5E0

        // Idempotent: NativeActivity already dlopened it for android_main, but this
        // binds the Java_…_OverlayKeepAliveService_… symbols for this class too.
        init {
            System.loadLibrary("avatar_vk_android")
        }

        fun start(ctx: Context) {
            val i = Intent(ctx, OverlayKeepAliveService::class.java)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                ctx.startForegroundService(i)
            } else {
                ctx.startService(i)
            }
        }

        fun stop(ctx: Context) {
            ctx.stopService(Intent(ctx, OverlayKeepAliveService::class.java))
        }
    }
}
