package com.izzy2lost.x1box

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.util.Log
import android.widget.Toast
import java.io.File

class LauncherActivity : Activity() {
  companion object {
    private const val TAG = "LauncherActivity"
  }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)

    val prefs = getSharedPreferences("x1box_prefs", MODE_PRIVATE)
    var setupComplete = prefs.getBoolean("setup_complete", false)
    val mcpxUriStr = prefs.getString("mcpxUri", null)
    val flashUriStr = prefs.getString("flashUri", null)
    val hddUriStr = prefs.getString("hddUri", null)
    val dvdUriStr = prefs.getString("dvdUri", null)
    val gamesFolderUriStr = prefs.getString("gamesFolderUri", null)
    val mcpxPath = prefs.getString("mcpxPath", null)
    val flashPath = prefs.getString("flashPath", null)
    val hddPath = prefs.getString("hddPath", null)
    val dvdPath = prefs.getString("dvdPath", null)

    val mcpxUri = mcpxUriStr?.let(Uri::parse)
    val flashUri = flashUriStr?.let(Uri::parse)
    val hddUri = hddUriStr?.let(Uri::parse)
    val dvdUri = dvdUriStr?.let(Uri::parse)
    val gamesFolderUri = gamesFolderUriStr?.let(Uri::parse)
    val frontendLaunch = FrontendLaunchHelper.resolve(this, intent, gamesFolderUri)

    val hasMcpx = hasLocalFile(mcpxPath) || (mcpxUri != null && hasPersistedReadPermission(mcpxUri))
    val hasFlash = hasLocalFile(flashPath) || (flashUri != null && hasPersistedReadPermission(flashUri))
    val hasHdd = hasLocalFile(hddPath) || (hddUri != null && hasPersistedReadPermission(hddUri))
    val hasDvd = hasLocalFile(dvdPath) || (dvdUri != null && hasPersistedReadPermission(dvdUri))
    val hasGamesFolder = gamesFolderUri != null && hasPersistedReadPermission(gamesFolderUri)

    val editor = prefs.edit()
    var clearedCore = false
    var clearedOptional = false
    if (!hasMcpx && mcpxUriStr != null) {
      editor.remove("mcpxUri")
      clearedCore = true
    }
    if (!hasFlash && flashUriStr != null) {
      editor.remove("flashUri")
      clearedCore = true
    }
    if (!hasHdd && hddUriStr != null) {
      editor.remove("hddUri")
      clearedCore = true
    }
    if (!hasDvd && dvdUriStr != null) {
      editor.remove("dvdUri")
      clearedOptional = true
    }
    if (!hasMcpx && mcpxPath != null) {
      editor.remove("mcpxPath")
      clearedCore = true
    }
    if (!hasFlash && flashPath != null) {
      editor.remove("flashPath")
      clearedCore = true
    }
    if (!hasHdd && hddPath != null) {
      editor.remove("hddPath")
      clearedCore = true
    }
    if (!hasDvd && dvdPath != null) {
      editor.remove("dvdPath")
      clearedOptional = true
    }
    if (!hasGamesFolder && gamesFolderUriStr != null) {
      editor.remove("gamesFolderUri")
      clearedCore = true
    }
    if (clearedCore) {
      setupComplete = false
      editor.putBoolean("setup_complete", false)
      editor.putBoolean("skip_game_picker", false)
      editor.apply()
    } else if (clearedOptional) {
      editor.apply()
    }

    if (frontendLaunch != null) {
      if (frontendLaunch.dvdUri != null) {
        FrontendLaunchHelper.persistReadPermission(this, intent, frontendLaunch.dvdUri)
      }
      prefs.edit()
        .putBoolean("skip_game_picker", false)
        .apply {
          when {
            frontendLaunch.dvdUri != null -> {
              putString("dvdUri", frontendLaunch.dvdUri.toString())
              remove("dvdPath")
            }
            frontendLaunch.dvdPath != null -> {
              putString("dvdPath", frontendLaunch.dvdPath)
              remove("dvdUri")
            }
          }
        }
        .apply()

      if (hasMcpx && hasFlash && hasHdd) {
        Log.i(TAG, "Frontend launch resolved via ${frontendLaunch.source}")
        startActivity(Intent(this, MainActivity::class.java))
        finish()
        return
      }

      Log.i(TAG, "Frontend launch queued, but core setup is incomplete")
      Toast.makeText(this, R.string.frontend_launch_setup_required, Toast.LENGTH_SHORT).show()
    } else if (hasExternalLaunchPayload(intent)) {
      Log.w(TAG, "Frontend intent received but no accessible game target was resolved")
      Toast.makeText(this, R.string.frontend_launch_unresolved, Toast.LENGTH_LONG).show()
    }

    val needsSetup = !setupComplete || !hasMcpx || !hasFlash || !hasHdd || !hasGamesFolder
    val next = if (needsSetup) SetupWizardActivity::class.java else GameLibraryActivity::class.java

    startActivity(Intent(this, next))
    finish()
  }

  private fun hasPersistedReadPermission(uri: Uri): Boolean {
    return contentResolver.persistedUriPermissions.any { perm ->
      perm.uri == uri && perm.isReadPermission
    }
  }

  private fun hasLocalFile(path: String?): Boolean {
    return path != null && File(path).isFile
  }

  private fun hasExternalLaunchPayload(intent: Intent?): Boolean {
    if (intent == null) {
      return false
    }
    if (intent.data != null || intent.clipData != null) {
      return true
    }
    return sequenceOf(
      Intent.EXTRA_STREAM,
      "rom",
      "ROM",
      "path",
      "PATH",
      "file",
      "FILE",
      "filename",
      "FILENAME",
      "romPath",
      "ROM_PATH",
      "uri",
      "URI",
    ).any { key -> intent.hasExtra(key) }
  }
}
