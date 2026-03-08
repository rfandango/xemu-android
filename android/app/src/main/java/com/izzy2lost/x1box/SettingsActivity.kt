package com.izzy2lost.x1box

import android.content.Context
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.widget.ArrayAdapter
import android.widget.AutoCompleteTextView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.button.MaterialButton
import com.google.android.material.button.MaterialButtonToggleGroup
import com.google.android.material.materialswitch.MaterialSwitch
import com.google.android.material.textfield.TextInputLayout
import java.io.File

class SettingsActivity : AppCompatActivity() {

  private val prefs by lazy { getSharedPreferences("x1box_prefs", Context.MODE_PRIVATE) }

  private data class EepromLanguageOption(
    val value: XboxEepromEditor.Language,
    val labelRes: Int,
  )

  private data class EepromVideoOption(
    val value: XboxEepromEditor.VideoStandard,
    val labelRes: Int,
  )

  private val eepromLanguageOptions = listOf(
    EepromLanguageOption(XboxEepromEditor.Language.ENGLISH, R.string.settings_eeprom_language_english),
    EepromLanguageOption(XboxEepromEditor.Language.JAPANESE, R.string.settings_eeprom_language_japanese),
    EepromLanguageOption(XboxEepromEditor.Language.GERMAN, R.string.settings_eeprom_language_german),
    EepromLanguageOption(XboxEepromEditor.Language.FRENCH, R.string.settings_eeprom_language_french),
    EepromLanguageOption(XboxEepromEditor.Language.SPANISH, R.string.settings_eeprom_language_spanish),
    EepromLanguageOption(XboxEepromEditor.Language.ITALIAN, R.string.settings_eeprom_language_italian),
    EepromLanguageOption(XboxEepromEditor.Language.KOREAN, R.string.settings_eeprom_language_korean),
    EepromLanguageOption(XboxEepromEditor.Language.CHINESE, R.string.settings_eeprom_language_chinese),
    EepromLanguageOption(XboxEepromEditor.Language.PORTUGUESE, R.string.settings_eeprom_language_portuguese),
  )

  private val eepromVideoOptions = listOf(
    EepromVideoOption(XboxEepromEditor.VideoStandard.NTSC_M, R.string.settings_eeprom_video_standard_ntsc_m),
    EepromVideoOption(XboxEepromEditor.VideoStandard.NTSC_J, R.string.settings_eeprom_video_standard_ntsc_j),
    EepromVideoOption(XboxEepromEditor.VideoStandard.PAL_I, R.string.settings_eeprom_video_standard_pal_i),
    EepromVideoOption(XboxEepromEditor.VideoStandard.PAL_M, R.string.settings_eeprom_video_standard_pal_m),
  )

  private var pendingVulkanUri: String? = null
  private var pendingVulkanName: String? = null
  private var clearVulkan = false

  private lateinit var tvVulkanDriverName: TextView
  private lateinit var tvEepromStatus: TextView
  private lateinit var inputEepromLanguage: TextInputLayout
  private lateinit var inputEepromVideoStandard: TextInputLayout
  private lateinit var dropdownEepromLanguage: AutoCompleteTextView
  private lateinit var dropdownEepromVideoStandard: AutoCompleteTextView

  private var selectedEepromLanguage = XboxEepromEditor.Language.ENGLISH
  private var selectedEepromVideoStandard = XboxEepromEditor.VideoStandard.NTSC_M
  private var eepromEditable = false
  private var eepromMissing = false
  private var eepromError = false

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
    EdgeToEdgeHelper.enable(this)
    EdgeToEdgeHelper.applySystemBarPadding(findViewById(R.id.settings_scroll))

    val toggleGraphicsApi = findViewById<MaterialButtonToggleGroup>(R.id.toggle_graphics_api)
    val toggleFiltering   = findViewById<MaterialButtonToggleGroup>(R.id.toggle_filtering)
    val toggleScale       = findViewById<MaterialButtonToggleGroup>(R.id.toggle_resolution_scale)
    val btn1x             = findViewById<MaterialButton>(R.id.btn_scale_1x)
    val btn2x             = findViewById<MaterialButton>(R.id.btn_scale_2x)
    val btn3x             = findViewById<MaterialButton>(R.id.btn_scale_3x)
    val toggleDisplayMode = findViewById<MaterialButtonToggleGroup>(R.id.toggle_display_mode)
    val toggleFrameRate   = findViewById<MaterialButtonToggleGroup>(R.id.toggle_frame_rate)
    val toggleSystemMemory = findViewById<MaterialButtonToggleGroup>(R.id.toggle_system_memory)
    val toggleThread      = findViewById<MaterialButtonToggleGroup>(R.id.toggle_tcg_thread)
    val btnMulti          = findViewById<MaterialButton>(R.id.btn_thread_multi)
    val btnSingle         = findViewById<MaterialButton>(R.id.btn_thread_single)
    val switchDsp         = findViewById<MaterialSwitch>(R.id.switch_use_dsp)
    val switchHrtf        = findViewById<MaterialSwitch>(R.id.switch_hrtf)
    val switchShaders     = findViewById<MaterialSwitch>(R.id.switch_cache_shaders)
    val switchFpu         = findViewById<MaterialSwitch>(R.id.switch_hard_fpu)
    val switchVsync       = findViewById<MaterialSwitch>(R.id.switch_vsync)
    val switchSkipBootAnim = findViewById<MaterialSwitch>(R.id.switch_skip_boot_anim)
    val toggleAudioDriver = findViewById<MaterialButtonToggleGroup>(R.id.toggle_audio_driver)
    val btnSave           = findViewById<MaterialButton>(R.id.btn_settings_save)
    tvVulkanDriverName    = findViewById(R.id.tv_vulkan_driver_name)
    val btnVulkanBrowse   = findViewById<MaterialButton>(R.id.btn_vulkan_browse)
    val btnVulkanClear    = findViewById<MaterialButton>(R.id.btn_vulkan_clear)
    tvEepromStatus        = findViewById(R.id.tv_eeprom_status)
    inputEepromLanguage   = findViewById(R.id.input_eeprom_language)
    inputEepromVideoStandard = findViewById(R.id.input_eeprom_video_standard)
    dropdownEepromLanguage = findViewById(R.id.dropdown_eeprom_language)
    dropdownEepromVideoStandard = findViewById(R.id.dropdown_eeprom_video_standard)

    // Load current values
    val renderer = prefs.getString("setting_renderer", "opengl") ?: "opengl"
    if (renderer == "opengl") {
      toggleGraphicsApi.check(R.id.btn_renderer_opengl)
    } else {
      toggleGraphicsApi.check(R.id.btn_renderer_vulkan)
    }

    val filtering = prefs.getString("setting_filtering", "linear") ?: "linear"
    if (filtering == "nearest") {
      toggleFiltering.check(R.id.btn_filtering_nearest)
    } else {
      toggleFiltering.check(R.id.btn_filtering_linear)
    }

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

    val systemMemoryMiB = prefs.getInt("setting_system_memory_mib", 64)
    when (systemMemoryMiB) {
      128  -> toggleSystemMemory.check(R.id.btn_memory_128)
      else -> toggleSystemMemory.check(R.id.btn_memory_64)
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
    switchVsync.isChecked   = prefs.getBoolean("setting_vsync", true)
    switchSkipBootAnim.isChecked =
      prefs.getBoolean("setting_skip_boot_anim", false)

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

    setupEepromEditor()

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
      val selectedSystemMemoryMiB = when (toggleSystemMemory.checkedButtonId) {
        R.id.btn_memory_128 -> 128
        else                -> 64
      }
      val selectedAudioDriver = when (toggleAudioDriver.checkedButtonId) {
        R.id.btn_audio_aaudio    -> "aaudio"
        R.id.btn_audio_disabled  -> "dummy"
        else                     -> "openslES"
      }
      val selectedRenderer = when (toggleGraphicsApi.checkedButtonId) {
        R.id.btn_renderer_opengl -> "opengl"
        else                     -> "vulkan"
      }
      val selectedFiltering = when (toggleFiltering.checkedButtonId) {
        R.id.btn_filtering_nearest -> "nearest"
        else                       -> "linear"
      }

      val edit = prefs.edit()
        .putInt("setting_display_mode", selectedDisplayMode)
        .putInt("setting_surface_scale", selectedScale)
        .putInt("setting_frame_rate_limit", selectedFrameRate)
        .putInt("setting_system_memory_mib", selectedSystemMemoryMiB)
        .putString("setting_tcg_thread", selectedThread)
        .putBoolean("setting_use_dsp", switchDsp.isChecked)
        .putBoolean("setting_hrtf", switchHrtf.isChecked)
        .putBoolean("setting_cache_shaders", switchShaders.isChecked)
        .putBoolean("setting_hard_fpu", switchFpu.isChecked)
        .putBoolean("setting_vsync", switchVsync.isChecked)
        .putBoolean("setting_skip_boot_anim", switchSkipBootAnim.isChecked)
        .putString("setting_audio_driver", selectedAudioDriver)
        .putString("setting_filtering", selectedFiltering)
        .putString("setting_renderer", selectedRenderer)

      when {
        clearVulkan -> edit
          .remove("setting_vulkan_driver_uri")
          .remove("setting_vulkan_driver_name")
        pendingVulkanUri != null -> edit
          .putString("setting_vulkan_driver_uri", pendingVulkanUri)
          .putString("setting_vulkan_driver_name", pendingVulkanName)
      }

      edit.apply()

      val toastResult = applyEepromEdits()
      Toast.makeText(this, toastResult.first, toastResult.second).show()
      finish()
    }
  }

  private fun setupEepromEditor() {
    val languageLabels = eepromLanguageOptions.map { getString(it.labelRes) }
    val videoLabels = eepromVideoOptions.map { getString(it.labelRes) }

    dropdownEepromLanguage.setAdapter(
      ArrayAdapter(this, android.R.layout.simple_list_item_1, languageLabels)
    )
    dropdownEepromVideoStandard.setAdapter(
      ArrayAdapter(this, android.R.layout.simple_list_item_1, videoLabels)
    )

    dropdownEepromLanguage.setOnItemClickListener { _, _, position, _ ->
      selectedEepromLanguage = eepromLanguageOptions[position].value
    }
    dropdownEepromVideoStandard.setOnItemClickListener { _, _, position, _ ->
      selectedEepromVideoStandard = eepromVideoOptions[position].value
    }

    val eepromFile = resolveEepromFile()
    if (!eepromFile.isFile) {
      eepromEditable = false
      eepromMissing = true
      eepromError = false
      setEepromEditorEnabled(false)
      setEepromLanguageSelection(selectedEepromLanguage)
      setEepromVideoSelection(selectedEepromVideoStandard)
      tvEepromStatus.text = getString(
        R.string.settings_eeprom_status_missing,
        eepromFile.absolutePath,
      )
      return
    }

    try {
      val snapshot = XboxEepromEditor.load(eepromFile)
      eepromEditable = true
      eepromMissing = false
      eepromError = false
      setEepromEditorEnabled(true)
      setEepromLanguageSelection(snapshot.language)
      setEepromVideoSelection(snapshot.videoStandard)

      val hasUnknownValues =
        snapshot.rawLanguage != snapshot.language.id ||
        snapshot.rawVideoStandard != snapshot.videoStandard.id
      tvEepromStatus.text = if (hasUnknownValues) {
        getString(R.string.settings_eeprom_status_unknown, eepromFile.absolutePath)
      } else {
        getString(R.string.settings_eeprom_status_ready, eepromFile.absolutePath)
      }
    } catch (_: IllegalArgumentException) {
      eepromEditable = false
      eepromMissing = false
      eepromError = true
      setEepromEditorEnabled(false)
      setEepromLanguageSelection(selectedEepromLanguage)
      setEepromVideoSelection(selectedEepromVideoStandard)
      tvEepromStatus.text = getString(
        R.string.settings_eeprom_status_invalid,
        eepromFile.absolutePath,
      )
    } catch (_: Exception) {
      eepromEditable = false
      eepromMissing = false
      eepromError = true
      setEepromEditorEnabled(false)
      setEepromLanguageSelection(selectedEepromLanguage)
      setEepromVideoSelection(selectedEepromVideoStandard)
      tvEepromStatus.text = getString(
        R.string.settings_eeprom_status_error,
        eepromFile.absolutePath,
      )
    }
  }

  private fun applyEepromEdits(): Pair<Int, Int> {
    if (eepromMissing) {
      return Pair(R.string.settings_saved_eeprom_missing, Toast.LENGTH_LONG)
    }
    if (eepromError || !eepromEditable) {
      return Pair(R.string.settings_saved_eeprom_failed, Toast.LENGTH_LONG)
    }

    return try {
      val changed = XboxEepromEditor.apply(
        resolveEepromFile(),
        selectedEepromLanguage,
        selectedEepromVideoStandard,
      )
      if (changed) {
        Pair(R.string.settings_saved_with_eeprom, Toast.LENGTH_SHORT)
      } else {
        Pair(R.string.settings_saved, Toast.LENGTH_SHORT)
      }
    } catch (_: Exception) {
      Pair(R.string.settings_saved_eeprom_failed, Toast.LENGTH_LONG)
    }
  }

  private fun setEepromEditorEnabled(enabled: Boolean) {
    inputEepromLanguage.isEnabled = enabled
    inputEepromVideoStandard.isEnabled = enabled
    dropdownEepromLanguage.isEnabled = enabled
    dropdownEepromVideoStandard.isEnabled = enabled
  }

  private fun setEepromLanguageSelection(language: XboxEepromEditor.Language) {
    selectedEepromLanguage = language
    val option = eepromLanguageOptions.firstOrNull { it.value == language }
      ?: eepromLanguageOptions.first()
    dropdownEepromLanguage.setText(getString(option.labelRes), false)
  }

  private fun setEepromVideoSelection(video: XboxEepromEditor.VideoStandard) {
    selectedEepromVideoStandard = video
    val option = eepromVideoOptions.firstOrNull { it.value == video }
      ?: eepromVideoOptions.first()
    dropdownEepromVideoStandard.setText(getString(option.labelRes), false)
  }

  private fun resolveEepromFile(): File {
    val base = getExternalFilesDir(null) ?: filesDir
    return File(File(base, "x1box"), "eeprom.bin")
  }

  private fun getFileName(uri: Uri): String? {
    return contentResolver.query(uri, null, null, null, null)?.use { cursor ->
      val col = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
      if (col >= 0 && cursor.moveToFirst()) cursor.getString(col) else null
    }
  }
}
