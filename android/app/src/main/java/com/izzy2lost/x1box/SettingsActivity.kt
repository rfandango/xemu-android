package com.izzy2lost.x1box

import android.content.Context
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.button.MaterialButton
import com.google.android.material.button.MaterialButtonToggleGroup
import com.google.android.material.materialswitch.MaterialSwitch

class SettingsActivity : AppCompatActivity() {

  private val prefs by lazy { getSharedPreferences("x1box_prefs", Context.MODE_PRIVATE) }

  private var pendingVulkanUri: String? = null
  private var pendingVulkanName: String? = null
  private var clearVulkan = false

  private lateinit var tvVulkanDriverName: TextView

  private val pickDriver =
    registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
      uri ?: return@registerForActivityResult
      pendingVulkanUri  = uri.toString()
      pendingVulkanName = getFileName(uri) ?: uri.lastPathSegment ?: "custom_driver.so"
      clearVulkan = false
      tvVulkanDriverName.text = pendingVulkanName
    }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    setContentView(R.layout.activity_settings)

    val toggleScale       = findViewById<MaterialButtonToggleGroup>(R.id.toggle_resolution_scale)
    val btn1x             = findViewById<MaterialButton>(R.id.btn_scale_1x)
    val btn2x             = findViewById<MaterialButton>(R.id.btn_scale_2x)
    val btn3x             = findViewById<MaterialButton>(R.id.btn_scale_3x)
    val toggleDisplayMode = findViewById<MaterialButtonToggleGroup>(R.id.toggle_display_mode)
    val toggleFrameRate   = findViewById<MaterialButtonToggleGroup>(R.id.toggle_frame_rate)
    val toggleThread      = findViewById<MaterialButtonToggleGroup>(R.id.toggle_tcg_thread)
    val btnMulti          = findViewById<MaterialButton>(R.id.btn_thread_multi)
    val btnSingle         = findViewById<MaterialButton>(R.id.btn_thread_single)
    val switchDsp         = findViewById<MaterialSwitch>(R.id.switch_use_dsp)
    val switchHrtf        = findViewById<MaterialSwitch>(R.id.switch_hrtf)
    val switchShaders     = findViewById<MaterialSwitch>(R.id.switch_cache_shaders)
    val switchFpu         = findViewById<MaterialSwitch>(R.id.switch_hard_fpu)
    val toggleAudioDriver = findViewById<MaterialButtonToggleGroup>(R.id.toggle_audio_driver)
    val btnSave           = findViewById<MaterialButton>(R.id.btn_settings_save)
    tvVulkanDriverName    = findViewById(R.id.tv_vulkan_driver_name)
    val btnVulkanBrowse   = findViewById<MaterialButton>(R.id.btn_vulkan_browse)
    val btnVulkanClear    = findViewById<MaterialButton>(R.id.btn_vulkan_clear)

    // Load current values
    val scale = prefs.getInt("setting_surface_scale", 1)
    when (scale) {
      2    -> toggleScale.check(R.id.btn_scale_2x)
      3    -> toggleScale.check(R.id.btn_scale_3x)
      else -> toggleScale.check(R.id.btn_scale_1x)
    }

    val displayMode = prefs.getInt("setting_display_mode", 0)
    when (displayMode) {
      1    -> toggleDisplayMode.check(R.id.btn_display_4_3)
      2    -> toggleDisplayMode.check(R.id.btn_display_16_9)
      else -> toggleDisplayMode.check(R.id.btn_display_stretch)
    }

    val frameRateLimit = prefs.getInt("setting_frame_rate_limit", 60)
    when (frameRateLimit) {
      30   -> toggleFrameRate.check(R.id.btn_fps_30)
      else -> toggleFrameRate.check(R.id.btn_fps_60)
    }

    tvVulkanDriverName.text =
      prefs.getString("setting_vulkan_driver_name", null)
        ?: getString(R.string.settings_vulkan_driver_none)

    val tcgThread = prefs.getString("setting_tcg_thread", "multi") ?: "multi"
    if (tcgThread == "single") {
      toggleThread.check(R.id.btn_thread_single)
    } else {
      toggleThread.check(R.id.btn_thread_multi)
    }

    switchDsp.isChecked     = prefs.getBoolean("setting_use_dsp", false)
    switchHrtf.isChecked    = prefs.getBoolean("setting_hrtf", true)
    switchShaders.isChecked = prefs.getBoolean("setting_cache_shaders", true)
    switchFpu.isChecked     = prefs.getBoolean("setting_hard_fpu", true)

    val audioDriver = prefs.getString("setting_audio_driver", "openslES") ?: "openslES"
    when (audioDriver) {
      "aaudio"  -> toggleAudioDriver.check(R.id.btn_audio_aaudio)
      "dummy"   -> toggleAudioDriver.check(R.id.btn_audio_disabled)
      else      -> toggleAudioDriver.check(R.id.btn_audio_opensles)
    }

    btnVulkanBrowse.setOnClickListener {
      pickDriver.launch(arrayOf("*/*"))
    }

    btnVulkanClear.setOnClickListener {
      pendingVulkanUri  = null
      pendingVulkanName = null
      clearVulkan = true
      tvVulkanDriverName.text = getString(R.string.settings_vulkan_driver_none)
    }

    btnSave.setOnClickListener {
      val selectedDisplayMode = when (toggleDisplayMode.checkedButtonId) {
        R.id.btn_display_4_3  -> 1
        R.id.btn_display_16_9 -> 2
        else                   -> 0
      }
      val selectedScale = when (toggleScale.checkedButtonId) {
        R.id.btn_scale_2x -> 2
        R.id.btn_scale_3x -> 3
        else              -> 1
      }
      val selectedFrameRate = when (toggleFrameRate.checkedButtonId) {
        R.id.btn_fps_30 -> 30
        R.id.btn_fps_60 -> 60
        else            -> 60
      }
      val selectedThread = when (toggleThread.checkedButtonId) {
        R.id.btn_thread_single -> "single"
        else                   -> "multi"
      }
      val selectedAudioDriver = when (toggleAudioDriver.checkedButtonId) {
        R.id.btn_audio_aaudio    -> "aaudio"
        R.id.btn_audio_disabled  -> "dummy"
        else                     -> "openslES"
      }

      val edit = prefs.edit()
        .putInt("setting_display_mode", selectedDisplayMode)
        .putInt("setting_surface_scale", selectedScale)
        .putInt("setting_frame_rate_limit", selectedFrameRate)
        .putString("setting_tcg_thread", selectedThread)
        .putBoolean("setting_use_dsp", switchDsp.isChecked)
        .putBoolean("setting_hrtf", switchHrtf.isChecked)
        .putBoolean("setting_cache_shaders", switchShaders.isChecked)
        .putBoolean("setting_hard_fpu", switchFpu.isChecked)
        .putString("setting_audio_driver", selectedAudioDriver)

      when {
        clearVulkan -> edit
          .remove("setting_vulkan_driver_uri")
          .remove("setting_vulkan_driver_name")
        pendingVulkanUri != null -> edit
          .putString("setting_vulkan_driver_uri", pendingVulkanUri)
          .putString("setting_vulkan_driver_name", pendingVulkanName)
      }

      edit.apply()

      Toast.makeText(this, R.string.settings_saved, Toast.LENGTH_SHORT).show()
      finish()
    }
  }

  private fun getFileName(uri: Uri): String? {
    return contentResolver.query(uri, null, null, null, null)?.use { cursor ->
      val col = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
      if (col >= 0 && cursor.moveToFirst()) cursor.getString(col) else null
    }
  }
}
