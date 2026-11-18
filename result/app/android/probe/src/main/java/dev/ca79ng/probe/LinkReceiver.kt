package dev.ca79ng.probe

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/**
 * Counts `jp.co.kawai.kwmcore.link` broadcasts (the original panel app's MIDI-port hand-over
 * signal). Records only; never answers, never
 * sends anything, never touches MIDI.
 */
class LinkReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        record(context, "manifest", intent)
    }

    companion object {
        const val ACTION = "jp.co.kawai.kwmcore.link"
        const val PREFS = "link"

        fun describe(intent: Intent): String {
            val extras = intent.extras ?: return "(no extras)"
            return extras.keySet().sorted().joinToString(", ") { k -> "$k=${extras.get(k)}" }
        }

        fun record(context: Context, via: String, intent: Intent) {
            val p = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            p.edit()
                .putInt("count_$via", p.getInt("count_$via", 0) + 1)
                .putLong("last_time", System.currentTimeMillis())
                .putString("last_via", via)
                .putString("last_extras", describe(intent))
                .apply()
        }
    }
}
