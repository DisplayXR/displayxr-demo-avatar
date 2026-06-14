// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// #558 overlay mode: a minimal foreground service whose only job is to keep the
// avatar process off the Android background freezer while its Activity is in the
// background (MainActivity.moveTaskToBack). The OpenXR render loop (in the
// NativeActivity's android_main thread) then keeps weaving the tiger into the
// runtime's service-owned TYPE_APPLICATION_OVERLAY over the LIVE launcher.
//
// It hosts no rendering itself — the NativeActivity still owns the native lib +
// render thread; this service just holds the process foreground so it isn't
// frozen. Gated on debug.dxr.overlay (MainActivity starts it only then).

package com.displayxr.avatar_vk_android

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder

class OverlayKeepAliveService : Service() {
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
        return START_STICKY
    }

    companion object {
        private const val NOTIF_ID = 0x5E0

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
