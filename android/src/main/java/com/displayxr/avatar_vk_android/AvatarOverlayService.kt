// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// Keep-alive foreground service for click-through overlay mode (runtime#1110).
//
// In overlay mode the Activity leaves the foreground task on purpose — that is
// the whole point, because an Activity in the foreground task parks a
// display-wide ActivityRecordInputSink that swallows every tap meant for the
// launcher underneath, whatever our own window's frame or alpha is. With no
// foreground Activity the process would be a background process, and two things
// break: Android is free to kill it, and CAMERA becomes a background access —
// which silently gives the in-process vendor face tracker no face and no error.
//
// This service fixes both and does nothing else. It owns no window: the overlay
// view is added through the application context's WindowManager, so it is
// independent of both this service and the Activity.
//
// foregroundServiceType=camera is the honest type: in Architecture A the vendor
// face tracker opens the front camera in THIS process.

package com.displayxr.avatar_vk_android

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.os.Build
import android.os.IBinder

class AvatarOverlayService : Service() {

    companion object {
        private const val CHANNEL_ID = "avatar_overlay"
        private const val NOTIFICATION_ID = 1110
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForeground(NOTIFICATION_ID, buildNotification())
        // Deliberately NOT sticky: if the process dies the overlay is gone with
        // it, and a restarted bare service would have no renderer to keep alive.
        return START_NOT_STICKY
    }

    private fun buildNotification(): Notification {
        val nm = getSystemService(NotificationManager::class.java)
        if (Build.VERSION.SDK_INT >= 26) {
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL_ID, "Avatar", NotificationManager.IMPORTANCE_LOW),
            )
        }
        // Tapping the notification re-launches MainActivity, and a re-launch while
        // the overlay is up means "quit" (MainActivity.onNewIntent) — the app has
        // no window chrome of its own to close.
        val quit = Intent(this, MainActivity::class.java)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP)
        val pi = PendingIntent.getActivity(
            this, 0, quit,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("Leo is on your desktop")
            .setContentText("Tap to close")
            .setSmallIcon(R.mipmap.ic_launcher)
            .setContentIntent(pi)
            .setOngoing(true)
            .build()
    }
}
