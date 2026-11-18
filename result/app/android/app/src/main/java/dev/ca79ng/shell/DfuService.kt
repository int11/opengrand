package dev.ca79ng.shell

import android.app.Activity
import no.nordicsemi.android.dfu.DfuBaseService

/** Nordic DFU service; like the original it only sets the notification target and debug flag. */
class DfuService : DfuBaseService() {
    override fun getNotificationTarget(): Class<out Activity> = MainActivity::class.java
    override fun isDebug(): Boolean = BuildConfig.DEBUG
}
