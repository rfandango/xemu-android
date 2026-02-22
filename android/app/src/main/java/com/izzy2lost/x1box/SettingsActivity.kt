package com.izzy2lost.x1box

import android.net.Uri
import android.os.Bundle
import android.view.View
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.button.MaterialButton
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.materialswitch.MaterialSwitch

class SettingsActivity : AppCompatActivity() {
  private val prefs by lazy { getSharedPreferences("x1box_prefs", MODE_PRIVATE) }

  private lateinit var driverStatusText: TextView
  private lateinit var gpuNotSupportedText: TextView
  private lateinit var btnInstallDriver: MaterialButton
  private lateinit var btnSelectDriver: MaterialButton
  private lateinit var btnResetDriver: MaterialButton
  private lateinit var switchShowFps: MaterialSwitch
  private lateinit var switchCacheCode: MaterialSwitch

  private val pickDriverZip =
    registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
      if (uri != null) {
        installDriverFromUri(uri)
      }
    }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    setContentView(R.layout.activity_settings)

    driverStatusText = findViewById(R.id.settings_gpu_driver_status)
    gpuNotSupportedText = findViewById(R.id.settings_gpu_not_supported)
    btnInstallDriver = findViewById(R.id.btn_install_driver)
    btnSelectDriver = findViewById(R.id.btn_select_driver)
    btnResetDriver = findViewById(R.id.btn_reset_driver)
    switchShowFps = findViewById(R.id.switch_show_fps)
    switchCacheCode = findViewById(R.id.switch_cache_code)

    findViewById<View>(R.id.btn_settings_back).setOnClickListener { finish() }

    GpuDriverHelper.init(this)

    val supportsCustom = GpuDriverHelper.supportsCustomDriverLoading()

    if (!supportsCustom) {
      gpuNotSupportedText.visibility = View.VISIBLE
      btnInstallDriver.isEnabled = false
      btnSelectDriver.isEnabled = false
      btnResetDriver.isEnabled = false
    }

    btnInstallDriver.setOnClickListener {
      pickDriverZip.launch(arrayOf("application/zip", "application/octet-stream"))
    }

    btnSelectDriver.setOnClickListener {
      showDriverSelectionDialog()
    }

    btnResetDriver.setOnClickListener {
      confirmResetDriver()
    }

    switchShowFps.isChecked = prefs.getBoolean("show_fps", true)
    switchShowFps.setOnCheckedChangeListener { _, checked ->
      prefs.edit().putBoolean("show_fps", checked).apply()
    }

    switchCacheCode.isChecked = prefs.getBoolean("cache_code", true)
    switchCacheCode.setOnCheckedChangeListener { _, checked ->
      prefs.edit().putBoolean("cache_code", checked).apply()
    }

    refreshDriverStatus()
  }

  private fun refreshDriverStatus() {
    val name = GpuDriverHelper.getInstalledDriverName()
    if (name != null) {
      driverStatusText.text = getString(R.string.settings_gpu_driver_active, name)
    } else {
      driverStatusText.text = getString(R.string.settings_gpu_driver_system)
    }
  }

  private fun installDriverFromUri(uri: Uri) {
    Thread {
      val success = GpuDriverHelper.installDriverFromUri(this, uri)
      runOnUiThread {
        if (success) {
          Toast.makeText(this, getString(R.string.settings_gpu_driver_installed), Toast.LENGTH_SHORT).show()
          refreshDriverStatus()
        } else {
          Toast.makeText(this, getString(R.string.settings_gpu_driver_install_failed), Toast.LENGTH_SHORT).show()
        }
      }
    }.start()
  }

  private fun showDriverSelectionDialog() {
    val drivers = GpuDriverHelper.getAvailableDrivers()
    if (drivers.isEmpty()) {
      Toast.makeText(this, getString(R.string.settings_gpu_driver_none_available), Toast.LENGTH_SHORT).show()
      return
    }

    val labels = drivers.map { driver ->
      buildString {
        append(driver.name ?: "Unknown")
        if (!driver.description.isNullOrBlank()) {
          append("\n")
          append(driver.description)
        }
        if (!driver.author.isNullOrBlank()) {
          append("\nby ")
          append(driver.author)
        }
      }
    }.toTypedArray()

    MaterialAlertDialogBuilder(this)
      .setTitle(R.string.settings_gpu_driver_select_title)
      .setItems(labels) { _, which ->
        val selected = drivers[which]
        if (selected.path != null) {
          val zipFile = java.io.File(selected.path)
          val success = GpuDriverHelper.installDriver(zipFile)
          if (success) {
            Toast.makeText(this, getString(R.string.settings_gpu_driver_installed), Toast.LENGTH_SHORT).show()
            refreshDriverStatus()
          } else {
            Toast.makeText(this, getString(R.string.settings_gpu_driver_install_failed), Toast.LENGTH_SHORT).show()
          }
        }
      }
      .setNegativeButton(android.R.string.cancel, null)
      .show()
  }

  private fun confirmResetDriver() {
    MaterialAlertDialogBuilder(this)
      .setTitle(R.string.settings_gpu_driver_reset_title)
      .setMessage(R.string.settings_gpu_driver_reset_message)
      .setPositiveButton(R.string.settings_gpu_driver_reset) { _, _ ->
        GpuDriverHelper.installDefaultDriver()
        Toast.makeText(this, getString(R.string.settings_gpu_driver_reset_done), Toast.LENGTH_SHORT).show()
        refreshDriverStatus()
      }
      .setNegativeButton(android.R.string.cancel, null)
      .show()
  }
}
