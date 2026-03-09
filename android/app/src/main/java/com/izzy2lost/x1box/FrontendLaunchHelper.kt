package com.izzy2lost.x1box

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.DocumentsContract
import androidx.documentfile.provider.DocumentFile
import java.io.File
import java.util.Locale

object FrontendLaunchHelper {
  data class LaunchTarget(
    val dvdUri: Uri? = null,
    val dvdPath: String? = null,
    val source: String
  )

  private val stringExtraKeys = listOf(
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
  )

  fun resolve(context: Context, intent: Intent?, gamesFolderUri: Uri?): LaunchTarget? {
    if (intent == null) {
      return null
    }

    for ((label, rawValue) in collectCandidates(intent)) {
      val resolved = resolveCandidate(context, gamesFolderUri, rawValue, label)
      if (resolved != null) {
        return resolved
      }
    }
    return null
  }

  fun persistReadPermission(context: Context, intent: Intent?, uri: Uri) {
    if (intent == null) {
      return
    }
    val flags = intent.flags and
      (Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
    if (flags == 0) {
      return
    }
    try {
      context.contentResolver.takePersistableUriPermission(uri, flags)
    } catch (_: SecurityException) {
    } catch (_: IllegalArgumentException) {
    }
  }

  private fun collectCandidates(intent: Intent): List<Pair<String, Any>> {
    val candidates = ArrayList<Pair<String, Any>>()

    intent.data?.let { candidates += "intent.data" to it }
    getExtraStream(intent)?.let { candidates += "Intent.EXTRA_STREAM" to it }
    intent.clipData?.let { clipData ->
      for (index in 0 until clipData.itemCount) {
        clipData.getItemAt(index)?.uri?.let { uri ->
          candidates += "clipData[$index]" to uri
        }
      }
    }

    val extras = intent.extras
    if (extras != null) {
      for (key in stringExtraKeys) {
        when (val value = extras.get(key)) {
          is Uri -> candidates += "extra:$key" to value
          is String -> candidates += "extra:$key" to value
          is CharSequence -> candidates += "extra:$key" to value.toString()
        }
      }
    }

    return candidates
  }

  private fun resolveCandidate(
    context: Context,
    gamesFolderUri: Uri?,
    rawValue: Any,
    label: String
  ): LaunchTarget? {
    return when (rawValue) {
      is Uri -> resolveUri(context, gamesFolderUri, rawValue, label)
      is String -> resolveString(context, gamesFolderUri, rawValue, label)
      else -> null
    }
  }

  private fun resolveUri(
    context: Context,
    gamesFolderUri: Uri?,
    uri: Uri,
    label: String
  ): LaunchTarget? {
    return when (uri.scheme?.lowercase(Locale.ROOT)) {
      null, "" -> resolvePath(context, gamesFolderUri, uri.toString(), label)
      "file" -> resolvePath(context, gamesFolderUri, uri.path, label)
      else -> LaunchTarget(dvdUri = uri, source = label)
    }
  }

  private fun resolveString(
    context: Context,
    gamesFolderUri: Uri?,
    value: String,
    label: String
  ): LaunchTarget? {
    val trimmed = value.trim()
    if (trimmed.isEmpty()) {
      return null
    }

    if (trimmed.startsWith("/")) {
      return resolvePath(context, gamesFolderUri, trimmed, label)
    }

    val parsed = runCatching { Uri.parse(trimmed) }.getOrNull()
    if (parsed != null && !parsed.scheme.isNullOrBlank()) {
      return resolveUri(context, gamesFolderUri, parsed, label)
    }

    return resolvePath(context, gamesFolderUri, trimmed, label)
  }

  private fun resolvePath(
    context: Context,
    gamesFolderUri: Uri?,
    rawPath: String?,
    label: String
  ): LaunchTarget? {
    val path = rawPath?.trim()?.takeIf { it.isNotEmpty() } ?: return null
    val directFile = File(path)
    if (directFile.isFile && directFile.canRead()) {
      return LaunchTarget(dvdPath = directFile.absolutePath, source = label)
    }

    val treeMatch = resolvePathAgainstGamesFolder(context, gamesFolderUri, path)
    if (treeMatch != null) {
      return LaunchTarget(dvdUri = treeMatch, source = label)
    }

    return null
  }

  private fun resolvePathAgainstGamesFolder(
    context: Context,
    gamesFolderUri: Uri?,
    rawPath: String
  ): Uri? {
    val treeUri = gamesFolderUri ?: return null
    val treeRootPath = treeUriToFilesystemPath(treeUri) ?: return null
    val normalizedTree = normalizeFilesystemPath(treeRootPath)
    val normalizedFile = normalizeFilesystemPath(rawPath)
    val relativePath = when {
      normalizedFile == normalizedTree -> ""
      normalizedFile.startsWith("$normalizedTree/") ->
        normalizedFile.removePrefix("$normalizedTree/")
      else -> return null
    }

    var node = DocumentFile.fromTreeUri(context, treeUri) ?: return null
    if (relativePath.isEmpty()) {
      return node.takeIf { it.isFile }?.uri
    }

    for (segment in relativePath.split('/')) {
      if (segment.isEmpty()) {
        continue
      }
      node = node.findFile(segment) ?: return null
    }
    return node.takeIf { it.isFile }?.uri
  }

  private fun treeUriToFilesystemPath(treeUri: Uri): String? {
    val documentId = runCatching { DocumentsContract.getTreeDocumentId(treeUri) }.getOrNull()
      ?: return null
    val volume = documentId.substringBefore(':', "")
    val relative = documentId.substringAfter(':', "")
    return when {
      volume.equals("primary", ignoreCase = true) -> buildPath("/storage/emulated/0", relative)
      volume.equals("home", ignoreCase = true) -> buildPath("/storage/emulated/0/Documents", relative)
      volume.isNotEmpty() -> buildPath("/storage/$volume", relative)
      else -> null
    }
  }

  private fun buildPath(base: String, relative: String): String {
    if (relative.isBlank()) {
      return base
    }
    return "$base/${relative.trimStart('/')}"
  }

  private fun normalizeFilesystemPath(path: String): String {
    val absolute = File(path).absolutePath.replace('\\', '/')
    return if (absolute.length > 1 && absolute.endsWith("/")) {
      absolute.dropLast(1)
    } else {
      absolute
    }
  }

  @Suppress("DEPRECATION")
  private fun getExtraStream(intent: Intent): Uri? {
    return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
      intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java)
    } else {
      intent.getParcelableExtra(Intent.EXTRA_STREAM) as? Uri
    }
  }
}
