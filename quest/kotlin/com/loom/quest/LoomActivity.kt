package com.loom.quest

import android.app.NativeActivity
import android.content.Context
import android.net.wifi.WifiManager
import android.os.Bundle

/**
 * The entire Java/Kotlin surface of the Quest client.
 *
 * It exists for one reason: WIFI_MODE_FULL_LOW_LATENCY has no NDK equivalent, and
 * without it the WiFi stack power-saves between frames and adds tens of
 * milliseconds of jitter — which would quietly invalidate every latency number
 * M3 is supposed to produce. Subclassing NativeActivity lets us hold the lock in
 * Kotlin and skip a JNI bridge entirely; everything else lives in C++.
 */
class LoomActivity : NativeActivity() {
    private var wifiLock: WifiManager.WifiLock? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val wifi = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        wifiLock = wifi.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "loom:stream").apply {
            setReferenceCounted(false)
            acquire()
        }
    }

    override fun onDestroy() {
        wifiLock?.takeIf { it.isHeld }?.release()
        wifiLock = null

        super.onDestroy()
    }
}
