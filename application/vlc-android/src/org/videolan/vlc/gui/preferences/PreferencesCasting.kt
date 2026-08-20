/*
 * *************************************************************************
 *  PreferencesCasting.kt
 * **************************************************************************
 *  Copyright © 2018 VLC authors and VideoLAN
 *  Author: Geoffrey Métais
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *  ***************************************************************************
 */
package org.videolan.vlc.gui.preferences

import android.content.SharedPreferences
import android.util.Log
import android.os.Bundle
import androidx.lifecycle.lifecycleScope
import androidx.preference.Preference
import kotlinx.coroutines.launch
import org.videolan.resources.VLCInstance
import org.videolan.tools.AppScope
import org.videolan.tools.KEY_CASTING_AUDIO_ONLY
import org.videolan.tools.KEY_CASTING_PASSTHROUGH
import org.videolan.tools.KEY_CASTING_QUALITY
import org.videolan.tools.KEY_ENABLE_CASTING
import org.videolan.vlc.R
import org.videolan.vlc.RendererDelegate
import org.videolan.vlc.gui.helpers.restartMediaPlayer

private const val TAG = "VLC/PreferencesCasting"

class PreferencesCasting : BasePreferenceFragment(), SharedPreferences.OnSharedPreferenceChangeListener {

    override fun getTitleId() = R.string.casting_category

    override fun getXml() = R.xml.preferences_casting

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        preferenceScreen.sharedPreferences!!.registerOnSharedPreferenceChangeListener(this)
    }

    override fun onPreferenceTreeClick(preference: Preference): Boolean {
        return when (preference.key) {
            KEY_ENABLE_CASTING -> {
                (activity as PreferencesActivity).setRestartApp()
                true
            }
            else -> super.onPreferenceTreeClick(preference)
        }
    }
    override fun onSharedPreferenceChanged(sharedPreferences: SharedPreferences?, key: String?) {
        when (key) {
            // Apply the casting opt-in immediately. RendererDelegate is a process-wide
            // object and the restart offered above only recreates the Activity, so
            // without this the discoverers keep multicasting after the user turns
            // casting off (and stay off after turning it on, until connectivity flaps).
            KEY_ENABLE_CASTING -> {
                val enabled = sharedPreferences?.getBoolean(KEY_ENABLE_CASTING, false) == true
                // VLCInstance.getInstance throws on an incompatible CPU and AppScope has no
                // CoroutineExceptionHandler, so an uncaught throw here would kill the process.
                AppScope.launch {
                    runCatching { if (enabled) RendererDelegate.start() else RendererDelegate.stop() }
                            .onFailure { Log.w(TAG, "casting toggle failed", it) }
                }
            }
            KEY_CASTING_PASSTHROUGH, KEY_CASTING_QUALITY, KEY_CASTING_AUDIO_ONLY -> {
                lifecycleScope.launch {
                    VLCInstance.restart()
                    restartMediaPlayer()
                }
            }
        }
    }
}