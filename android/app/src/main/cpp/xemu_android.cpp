#include <SDL.h>
#include <SDL_main.h>
#include <SDL_system.h>

#include <GLES3/gl3.h>
#include <toml++/toml.h>

#include <android/log.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <jni.h>

#include <cctype>
#include <cstdint>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

#include "xemu-settings.h"

struct Error;
struct AddfdInfo;
extern "C" AddfdInfo* monitor_fdset_add_fd(int fd, bool has_fdset_id,
                                           int64_t fdset_id,
                                           const char* opaque,
                                           Error** errp);

namespace {
constexpr const char* kLogTag = "xemu-android";
constexpr const char* kPrefsName = "x1box_prefs";

static JNIEnv* GetEnv();
static jobject GetActivity(JNIEnv* env);
static bool HasException(JNIEnv* env, const char* context);

static void LogInfo(const char* msg) {
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "%s", msg);
}

static void LogInfoFmt(const char* fmt, const char* detail) {
  __android_log_print(ANDROID_LOG_INFO, kLogTag, fmt, detail);
}

static void LogInfoInt(const char* fmt, int value) {
  __android_log_print(ANDROID_LOG_INFO, kLogTag, fmt, value);
}

static void LogError(const char* msg) {
  __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", msg);
}

static void LogErrorInt(const char* fmt, int value) {
  __android_log_print(ANDROID_LOG_ERROR, kLogTag, fmt, value);
}

static void LogErrorFmt(const char* fmt, const char* detail) {
  __android_log_print(ANDROID_LOG_ERROR, kLogTag, fmt, detail);
}

static int g_next_dvd_fdset_id = 9000;

static bool EnsureDirExists(const std::string& path) {
  if (path.empty()) return false;
  if (mkdir(path.c_str(), 0755) == 0) return true;
  return errno == EEXIST;
}

static bool FileExists(const std::string& path) {
  if (path.empty()) return false;
  struct stat st {};
  return stat(path.c_str(), &st) == 0;
}

static bool IsTcgTuningEnabled() {
  const char* value = SDL_getenv("XEMU_ANDROID_TCG_TUNING");
  return !(value && value[0] == '0');
}

static void LoadGameControllerMappingsFromAssets() {
  constexpr const char* kDbAssetName = "gamecontrollerdb.txt";

  JNIEnv* env = GetEnv();
  jobject activity = GetActivity(env);
  if (!env || !activity) {
    LogInfo("Controller mappings: JNI unavailable");
    return;
  }

  jclass activityClass = env->GetObjectClass(activity);
  jmethodID getAssets = env->GetMethodID(
      activityClass, "getAssets", "()Landroid/content/res/AssetManager;");
  if (!getAssets) {
    LogInfo("Controller mappings: Activity.getAssets() not found");
    return;
  }

  jobject assetManagerObj = env->CallObjectMethod(activity, getAssets);
  if (HasException(env, "Activity.getAssets") || !assetManagerObj) {
    LogInfo("Controller mappings: could not access AssetManager");
    return;
  }

  AAssetManager* assetManager = AAssetManager_fromJava(env, assetManagerObj);
  env->DeleteLocalRef(assetManagerObj);
  if (!assetManager) {
    LogInfo("Controller mappings: AssetManager bridge failed");
    return;
  }

  AAsset* asset = AAssetManager_open(assetManager, kDbAssetName, AASSET_MODE_STREAMING);
  if (!asset) {
    LogInfo("Controller mappings: no custom gamecontrollerdb.txt in assets");
    return;
  }

  const off_t length = AAsset_getLength(asset);
  if (length <= 0 || length > INT_MAX) {
    AAsset_close(asset);
    LogError("Controller mappings: invalid gamecontrollerdb.txt size");
    return;
  }

  std::vector<char> data(static_cast<size_t>(length));
  size_t total = 0;
  while (total < data.size()) {
    const int read = AAsset_read(asset, data.data() + total,
                                 static_cast<size_t>(data.size() - total));
    if (read <= 0) {
      break;
    }
    total += static_cast<size_t>(read);
  }
  AAsset_close(asset);

  if (total == 0) {
    LogError("Controller mappings: gamecontrollerdb.txt is empty");
    return;
  }
  data.resize(total);

  SDL_RWops* rw = SDL_RWFromConstMem(data.data(), static_cast<int>(data.size()));
  if (!rw) {
    LogErrorFmt("Controller mappings: SDL_RWFromConstMem failed: %s", SDL_GetError());
    return;
  }

  const int added = SDL_GameControllerAddMappingsFromRW(rw, 1);
  if (added < 0) {
    LogErrorFmt("Controller mappings: failed to parse gamecontrollerdb.txt: %s", SDL_GetError());
    return;
  }

  LogInfoInt("Controller mappings loaded from assets: %d", added);
}

static const char* GetTcgThreadFromEnv() {
  const char* value = SDL_getenv("XEMU_ANDROID_TCG_THREAD");
  if (value && strcmp(value, "single") == 0) {
    return "single";
  }
  return "multi";
}

static int GetTcgTbSizeFromEnv() {
  constexpr int kDefaultTbSize = 128;
  constexpr int kMinTbSize = 32;
  constexpr int kMaxTbSize = 512;

  const char* value = SDL_getenv("XEMU_ANDROID_TCG_TB_SIZE");
  if (!value || value[0] == '\0') {
    return kDefaultTbSize;
  }

  char* end = nullptr;
  long parsed = strtol(value, &end, 10);
  if (end == value || (end && *end != '\0')) {
    return kDefaultTbSize;
  }
  if (parsed < kMinTbSize) {
    parsed = kMinTbSize;
  } else if (parsed > kMaxTbSize) {
    parsed = kMaxTbSize;
  }
  return static_cast<int>(parsed);
}

static std::string ToLowerAscii(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

static std::string ResolveAndroidAudioDriverHint() {
  // OpenSL ES is preferred over AAudio because AAudio exclusively requests
  // MMAP no-IRQ low-latency outputs (AUDIO_OUTPUT_FLAG_MMAP_NOIRQ). On some
  // devices the MMAP output count is capped and openDirectOutput fails when
  // the limit is reached, leaving the audio stream inactive. The standard
  // Android AudioTrack backend remains the safe fallback.
  constexpr const char* kDefaultAudioDriverHint = "openslES,android,dummy";
  const char* value = SDL_getenv("XEMU_ANDROID_AUDIO_DRIVER");
  if (!value || value[0] == '\0') {
    return kDefaultAudioDriverHint;
  }

  std::string raw(value);
  std::string normalized = ToLowerAscii(raw);
  if (normalized == "auto" || normalized == "default") {
    return kDefaultAudioDriverHint;
  }
  if (normalized == "opensl" || normalized == "opensles") {
    return "openslES,android,dummy";
  }
  if (normalized == "aaudio") {
    return "aaudio,android,dummy";
  }
  if (normalized == "android" || normalized == "audiotrack") {
    // Legacy recovery: a saved "android" preference should no longer force
    // AudioTrack first, because that path was found to exit immediately on
    // some devices. Fall back to the old OpenSL ES-first behavior.
    return "openslES,android,dummy";
  }
  if (normalized == "null" || normalized == "none" || normalized == "dummy") {
    return "dummy";
  }
  return raw;
}

static JNIEnv* GetEnv() {
  return static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
}

static jobject GetActivity(JNIEnv* env) {
  (void)env;
  return reinterpret_cast<jobject>(SDL_AndroidGetActivity());
}

static bool HasException(JNIEnv* env, const char* context) {
  if (!env->ExceptionCheck()) return false;
  env->ExceptionDescribe();
  env->ExceptionClear();
  LogErrorFmt("JNI exception in %s", context);
  return true;
}

static std::string JStringToString(JNIEnv* env, jstring value) {
  if (!value) return {};
  const char* utf = env->GetStringUTFChars(value, nullptr);
  if (!utf) return {};
  std::string out(utf);
  env->ReleaseStringUTFChars(value, utf);
  return out;
}

static bool HasInlineAioCrashFlag(const std::string& flag_path) {
  if (flag_path.empty()) {
    return false;
  }
  struct stat st {};
  return stat(flag_path.c_str(), &st) == 0;
}

static bool ShouldEnableInlineAioWorkaround(const std::string& crash_flag_path) {
  const char* forced = SDL_getenv("XEMU_ANDROID_INLINE_AIO");
  if (forced) {
    return forced[0] != '\0' && forced[0] != '0';
  }

  if (HasInlineAioCrashFlag(crash_flag_path)) {
    LogInfoFmt("Inline AIO enabled from crash marker: %s",
               crash_flag_path.c_str());
    return true;
  }

  return false;
}

static std::string GetPrefString(JNIEnv* env, jobject activity, const char* key) {
  if (!env || !activity || !key || key[0] == '\0') {
    return {};
  }

  std::string out;
  jclass activityClass = nullptr;
  jobject prefs = nullptr;
  jclass prefsClass = nullptr;
  jstring value = nullptr;
  jmethodID getPrefs = nullptr;
  jmethodID getString = nullptr;

  activityClass = env->GetObjectClass(activity);
  if (!activityClass) {
    return {};
  }

  getPrefs = env->GetMethodID(activityClass, "getSharedPreferences",
                              "(Ljava/lang/String;I)Landroid/content/SharedPreferences;");
  if (!getPrefs) {
    goto cleanup;
  }

  {
    jstring prefsName = env->NewStringUTF(kPrefsName);
    if (!prefsName) {
      goto cleanup;
    }
    prefs = env->CallObjectMethod(activity, getPrefs, prefsName, 0);
    env->DeleteLocalRef(prefsName);
  }
  if (HasException(env, "getSharedPreferences") || !prefs) {
    goto cleanup;
  }

  prefsClass = env->GetObjectClass(prefs);
  if (!prefsClass) {
    goto cleanup;
  }
  getString = env->GetMethodID(
      prefsClass, "getString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
  if (!getString) {
    goto cleanup;
  }

  {
    jstring jkey = env->NewStringUTF(key);
    if (!jkey) {
      goto cleanup;
    }
    jstring jdefault = nullptr;
    value = static_cast<jstring>(env->CallObjectMethod(prefs, getString, jkey, jdefault));
    env->DeleteLocalRef(jkey);
  }
  if (HasException(env, "SharedPreferences.getString")) {
    goto cleanup;
  }

  out = JStringToString(env, value);

cleanup:
  if (value) env->DeleteLocalRef(value);
  if (prefsClass) env->DeleteLocalRef(prefsClass);
  if (prefs) env->DeleteLocalRef(prefs);
  if (activityClass) env->DeleteLocalRef(activityClass);
  return out;
}

static bool GetPrefBool(JNIEnv* env, jobject activity, const char* key, bool defValue) {
  if (!env || !activity || !key || key[0] == '\0') {
    return defValue;
  }
  bool out = defValue;
  jclass activityClass = env->GetObjectClass(activity);
  if (!activityClass) return defValue;
  jmethodID getPrefs = env->GetMethodID(activityClass, "getSharedPreferences",
                                        "(Ljava/lang/String;I)Landroid/content/SharedPreferences;");
  env->DeleteLocalRef(activityClass);
  if (!getPrefs) return defValue;
  jstring prefsName = env->NewStringUTF(kPrefsName);
  if (!prefsName) return defValue;
  jobject prefs = env->CallObjectMethod(activity, getPrefs, prefsName, 0);
  env->DeleteLocalRef(prefsName);
  if (HasException(env, "getSharedPreferences") || !prefs) return defValue;
  jclass prefsClass = env->GetObjectClass(prefs);
  if (prefsClass) {
    jmethodID contains = env->GetMethodID(prefsClass, "contains", "(Ljava/lang/String;)Z");
    jmethodID getBool  = env->GetMethodID(prefsClass, "getBoolean", "(Ljava/lang/String;Z)Z");
    if (contains && getBool) {
      jstring jkey = env->NewStringUTF(key);
      if (jkey) {
        jboolean hasKey = env->CallBooleanMethod(prefs, contains, jkey);
        if (!HasException(env, "contains") && hasKey) {
          out = (bool)env->CallBooleanMethod(prefs, getBool, jkey, (jboolean)defValue);
          HasException(env, "getBoolean");
        }
        env->DeleteLocalRef(jkey);
      }
    }
    env->DeleteLocalRef(prefsClass);
  }
  env->DeleteLocalRef(prefs);
  return out;
}

static int GetPrefInt(JNIEnv* env, jobject activity, const char* key, int defValue) {
  if (!env || !activity || !key || key[0] == '\0') {
    return defValue;
  }
  int out = defValue;
  jclass activityClass = env->GetObjectClass(activity);
  if (!activityClass) return defValue;
  jmethodID getPrefs = env->GetMethodID(activityClass, "getSharedPreferences",
                                        "(Ljava/lang/String;I)Landroid/content/SharedPreferences;");
  env->DeleteLocalRef(activityClass);
  if (!getPrefs) return defValue;
  jstring prefsName = env->NewStringUTF(kPrefsName);
  if (!prefsName) return defValue;
  jobject prefs = env->CallObjectMethod(activity, getPrefs, prefsName, 0);
  env->DeleteLocalRef(prefsName);
  if (HasException(env, "getSharedPreferences") || !prefs) return defValue;
  jclass prefsClass = env->GetObjectClass(prefs);
  if (prefsClass) {
    jmethodID contains = env->GetMethodID(prefsClass, "contains", "(Ljava/lang/String;)Z");
    jmethodID getInt   = env->GetMethodID(prefsClass, "getInt", "(Ljava/lang/String;I)I");
    if (contains && getInt) {
      jstring jkey = env->NewStringUTF(key);
      if (jkey) {
        jboolean hasKey = env->CallBooleanMethod(prefs, contains, jkey);
        if (!HasException(env, "contains") && hasKey) {
          out = (int)env->CallIntMethod(prefs, getInt, jkey, (jint)defValue);
          HasException(env, "getInt");
        }
        env->DeleteLocalRef(jkey);
      }
    }
    env->DeleteLocalRef(prefsClass);
  }
  env->DeleteLocalRef(prefs);
  return out;
}

static bool IsSeekableFd(int fd) {
  errno = 0;
  return lseek(fd, 0, SEEK_CUR) != static_cast<off_t>(-1);
}

static bool CopyUriToPath(JNIEnv* env, jobject activity, const std::string& uriString, const std::string& path) {
  if (!env || !activity || uriString.empty() || path.empty()) return false;

  bool success = false;
  jclass activityClass = nullptr;
  jobject resolver = nullptr;
  jclass uriClass = nullptr;
  jobject uri = nullptr;
  jclass resolverClass = nullptr;
  jobject inputStream = nullptr;
  jclass fosClass = nullptr;
  jobject outputStream = nullptr;
  jclass inputClass = nullptr;
  jclass outputClass = nullptr;
  jbyteArray buffer = nullptr;
  jmethodID readMethod = nullptr;
  jmethodID writeMethod = nullptr;
  jmethodID closeInput = nullptr;
  jmethodID closeOutput = nullptr;

  activityClass = env->GetObjectClass(activity);
  if (!activityClass) {
    goto cleanup;
  }

  {
    jmethodID getContentResolver = env->GetMethodID(activityClass, "getContentResolver",
                                                    "()Landroid/content/ContentResolver;");
    if (!getContentResolver) {
      goto cleanup;
    }
    resolver = env->CallObjectMethod(activity, getContentResolver);
    if (HasException(env, "getContentResolver") || !resolver) {
      goto cleanup;
    }
  }

  uriClass = env->FindClass("android/net/Uri");
  if (!uriClass) {
    goto cleanup;
  }
  {
    jmethodID parse = env->GetStaticMethodID(uriClass, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    if (!parse) {
      goto cleanup;
    }
    jstring juri = env->NewStringUTF(uriString.c_str());
    if (!juri) {
      goto cleanup;
    }
    uri = env->CallStaticObjectMethod(uriClass, parse, juri);
    env->DeleteLocalRef(juri);
    if (HasException(env, "Uri.parse") || !uri) {
      goto cleanup;
    }
  }

  resolverClass = env->GetObjectClass(resolver);
  if (!resolverClass) {
    goto cleanup;
  }
  {
    jmethodID openInputStream = env->GetMethodID(
        resolverClass, "openInputStream", "(Landroid/net/Uri;)Ljava/io/InputStream;");
    if (!openInputStream) {
      goto cleanup;
    }
    inputStream = env->CallObjectMethod(resolver, openInputStream, uri);
    if (HasException(env, "openInputStream") || !inputStream) {
      goto cleanup;
    }
  }

  fosClass = env->FindClass("java/io/FileOutputStream");
  if (!fosClass) {
    goto cleanup;
  }
  {
    jmethodID fosCtor = env->GetMethodID(fosClass, "<init>", "(Ljava/lang/String;)V");
    if (!fosCtor) {
      goto cleanup;
    }
    jstring jpath = env->NewStringUTF(path.c_str());
    if (!jpath) {
      goto cleanup;
    }
    outputStream = env->NewObject(fosClass, fosCtor, jpath);
    env->DeleteLocalRef(jpath);
    if (HasException(env, "FileOutputStream.<init>") || !outputStream) {
      goto cleanup;
    }
  }

  inputClass = env->GetObjectClass(inputStream);
  outputClass = env->GetObjectClass(outputStream);
  if (!inputClass || !outputClass) {
    goto cleanup;
  }
  readMethod = env->GetMethodID(inputClass, "read", "([B)I");
  closeInput = env->GetMethodID(inputClass, "close", "()V");
  writeMethod = env->GetMethodID(outputClass, "write", "([BII)V");
  closeOutput = env->GetMethodID(outputClass, "close", "()V");
  if (!readMethod || !writeMethod || !closeInput || !closeOutput) {
    goto cleanup;
  }

  {
    const int kBufferSize = 64 * 1024;
    buffer = env->NewByteArray(kBufferSize);
    if (!buffer) {
      goto cleanup;
    }

    while (true) {
      jint read = env->CallIntMethod(inputStream, readMethod, buffer);
      if (HasException(env, "InputStream.read")) {
        goto cleanup;
      }
      if (read < 0) {
        break;
      }
      if (read == 0) {
        continue;
      }
      env->CallVoidMethod(outputStream, writeMethod, buffer, 0, read);
      if (HasException(env, "OutputStream.write")) {
        goto cleanup;
      }
    }
    success = true;
  }

cleanup:
  if (buffer) env->DeleteLocalRef(buffer);
  if (inputStream && closeInput) {
    env->CallVoidMethod(inputStream, closeInput);
    if (HasException(env, "InputStream.close")) {
      success = false;
    }
  }
  if (outputStream && closeOutput) {
    env->CallVoidMethod(outputStream, closeOutput);
    if (HasException(env, "OutputStream.close")) {
      success = false;
    }
  }
  if (outputClass) env->DeleteLocalRef(outputClass);
  if (inputClass) env->DeleteLocalRef(inputClass);
  if (outputStream) env->DeleteLocalRef(outputStream);
  if (fosClass) env->DeleteLocalRef(fosClass);
  if (inputStream) env->DeleteLocalRef(inputStream);
  if (resolverClass) env->DeleteLocalRef(resolverClass);
  if (uri) env->DeleteLocalRef(uri);
  if (uriClass) env->DeleteLocalRef(uriClass);
  if (resolver) env->DeleteLocalRef(resolver);
  if (activityClass) env->DeleteLocalRef(activityClass);
  return success;
}

static std::string OpenUriAsReadOnlyFdPath(JNIEnv* env,
                                           jobject activity,
                                           const std::string& uriString) {
  if (!env || !activity || uriString.empty()) {
    return {};
  }

  std::string out;
  jclass activityClass = nullptr;
  jobject resolver = nullptr;
  jclass resolverClass = nullptr;
  jclass uriClass = nullptr;
  jobject uri = nullptr;
  jobject parcelFd = nullptr;
  jclass parcelFdClass = nullptr;

  activityClass = env->GetObjectClass(activity);
  if (!activityClass) {
    goto cleanup;
  }

  {
    jmethodID getContentResolver = env->GetMethodID(
        activityClass, "getContentResolver",
        "()Landroid/content/ContentResolver;");
    if (!getContentResolver) {
      goto cleanup;
    }
    resolver = env->CallObjectMethod(activity, getContentResolver);
    if (HasException(env, "getContentResolver") || !resolver) {
      goto cleanup;
    }
  }

  uriClass = env->FindClass("android/net/Uri");
  if (!uriClass) {
    goto cleanup;
  }
  {
    jmethodID parse = env->GetStaticMethodID(
        uriClass, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    if (!parse) {
      goto cleanup;
    }
    jstring juri = env->NewStringUTF(uriString.c_str());
    if (!juri) {
      goto cleanup;
    }
    uri = env->CallStaticObjectMethod(uriClass, parse, juri);
    env->DeleteLocalRef(juri);
    if (HasException(env, "Uri.parse") || !uri) {
      goto cleanup;
    }
  }

  resolverClass = env->GetObjectClass(resolver);
  if (!resolverClass) {
    goto cleanup;
  }
  {
    jmethodID openFileDescriptor = env->GetMethodID(
        resolverClass, "openFileDescriptor",
        "(Landroid/net/Uri;Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;");
    if (!openFileDescriptor) {
      goto cleanup;
    }

    jstring readMode = env->NewStringUTF("r");
    if (!readMode) {
      goto cleanup;
    }
    parcelFd = env->CallObjectMethod(resolver, openFileDescriptor, uri, readMode);
    env->DeleteLocalRef(readMode);
    if (HasException(env, "openFileDescriptor") || !parcelFd) {
      goto cleanup;
    }
  }

  parcelFdClass = env->GetObjectClass(parcelFd);
  if (!parcelFdClass) {
    goto cleanup;
  }
  {
    jmethodID detachFd = env->GetMethodID(parcelFdClass, "detachFd", "()I");
    if (!detachFd) {
      goto cleanup;
    }
    jint fd = env->CallIntMethod(parcelFd, detachFd);
    if (HasException(env, "ParcelFileDescriptor.detachFd") || fd < 0) {
      goto cleanup;
    }
    if (!IsSeekableFd(fd)) {
      LogInfo("DVD descriptor is not seekable; falling back to staged copy");
      close(fd);
      goto cleanup;
    }

    {
      const int fdsetId = g_next_dvd_fdset_id++;
      AddfdInfo* fdinfo =
          monitor_fdset_add_fd(fd, true, fdsetId, "android-dvd", nullptr);
      if (!fdinfo) {
        LogError("Failed to register DVD fd with QEMU fdset");
        close(fd);
        goto cleanup;
      }
      out = "/dev/fdset/" + std::to_string(fdsetId);
    }
  }

cleanup:
  if (parcelFdClass) env->DeleteLocalRef(parcelFdClass);
  if (parcelFd) env->DeleteLocalRef(parcelFd);
  if (uri) env->DeleteLocalRef(uri);
  if (uriClass) env->DeleteLocalRef(uriClass);
  if (resolverClass) env->DeleteLocalRef(resolverClass);
  if (resolver) env->DeleteLocalRef(resolver);
  if (activityClass) env->DeleteLocalRef(activityClass);
  return out;
}

struct EmulatorSettings {
  int surface_scale    = 1;           // 1, 2, or 3
  int frame_rate_limit = 60;          // 30 or 60
  int system_memory_mib = 64;         // 64 or 128
  std::string tcg_thread = "multi";   // "single" or "multi"
  std::string renderer = "opengl";    // "vulkan" or "opengl"
  std::string filtering = "linear";   // "linear" or "nearest"
  bool use_dsp         = false;
  bool hrtf            = true;
  bool cache_shaders   = true;
  bool hard_fpu        = true;
  bool vsync           = true;
  bool skip_boot_anim  = false;
};

struct SetupFiles {
  std::string mcpx;
  std::string flash;
  std::string hdd;
  std::string dvd;
  std::string eeprom;
  std::string config_path;
  std::string inline_aio_flag_path;
  std::string audio_driver; // raw pref value, e.g. "openslES", "aaudio", "dummy"
};

static bool WriteConfigToml(const std::string& config_path,
                            const std::string& mcpx,
                            const std::string& flash,
                            const std::string& hdd,
                            const std::string& dvd,
                            const std::string& eeprom,
                            const EmulatorSettings& settings) {
  if (config_path.empty()) return false;
  toml::table tbl;

  if (FileExists(config_path)) {
    try {
      tbl = toml::parse_file(config_path);
    } catch (const toml::parse_error&) {
      // Ignore parse errors; we'll rewrite a clean config.
    }
  }

  auto EnsureTable = [](toml::table& parent, std::string_view key) -> toml::table* {
    if (auto* node = parent.get(key)) {
      if (auto* existing = node->as_table()) {
        return existing;
      }
    }
    parent.insert_or_assign(key, toml::table{});
    return parent.get(key)->as_table();
  };

  toml::table* general = EnsureTable(tbl, "general");
  toml::table* display = EnsureTable(tbl, "display");
  toml::table* display_quality = EnsureTable(*display, "quality");
  toml::table* display_window = EnsureTable(*display, "window");
  toml::table* audio = EnsureTable(tbl, "audio");
  toml::table* audio_vp = EnsureTable(*audio, "vp");
  toml::table* android = EnsureTable(tbl, "android");
  toml::table* sys = EnsureTable(tbl, "sys");
  toml::table* perf = EnsureTable(tbl, "perf");
  toml::table* files = EnsureTable(*sys, "files");
  if (!general || !display || !display_quality || !display_window || !audio ||
      !audio_vp || !android || !sys || !perf || !files) {
    LogErrorFmt("Failed to build config tables at %s", config_path.c_str());
    return false;
  }

  general->insert_or_assign("show_welcome", false);
  general->insert_or_assign("skip_boot_anim", settings.skip_boot_anim);
  {
    const std::string r = (settings.renderer == "opengl") ? "opengl" : "vulkan";
    display->insert_or_assign("renderer", r);
  }
  display->insert_or_assign(
      "filtering", settings.filtering == "nearest" ? "nearest" : "linear");
  display_window->insert_or_assign("vsync", settings.vsync);
  // User-controlled settings (always written so they take effect immediately)
  {
    int scale = settings.surface_scale;
    if (scale < 1) scale = 1;
    if (scale > 3) scale = 3;
    display_quality->insert_or_assign("surface_scale", scale);
  }
  audio->insert_or_assign("use_dsp", settings.use_dsp);
  audio->insert_or_assign("hrtf", settings.hrtf);
  perf->insert_or_assign("cache_shaders", settings.cache_shaders);
  perf->insert_or_assign("hard_fpu", settings.hard_fpu);
  android->insert_or_assign("tcg_thread",
    (settings.tcg_thread == "single") ? "single" : "multi");
  {
    int frame_rate_limit = settings.frame_rate_limit;
    if (frame_rate_limit != 30 && frame_rate_limit != 60) {
      frame_rate_limit = 60;
    }
    android->insert_or_assign("frame_rate_limit", frame_rate_limit);
  }
  if (!audio_vp->contains("num_workers")) {
    audio_vp->insert_or_assign("num_workers", 0);
  }
  if (!audio->contains("volume_limit")) {
    audio->insert_or_assign("volume_limit", 1.0);
  }
  if (!android->contains("tcg_tuning")) {
    android->insert_or_assign("tcg_tuning", true);
  }
  if (!android->contains("tcg_tb_size")) {
    android->insert_or_assign("tcg_tb_size", 128);
  }
  if (!android->contains("audio_driver")) {
    android->insert_or_assign("audio_driver", "openslES");
  }
  sys->insert_or_assign(
      "mem_limit", settings.system_memory_mib == 128 ? "128" : "64");

  files->insert_or_assign("bootrom_path", mcpx);
  files->insert_or_assign("flashrom_path", flash);
  files->insert_or_assign("eeprom_path", eeprom);
  files->insert_or_assign("hdd_path", hdd);
  files->insert_or_assign("dvd_path", dvd);

  std::ofstream out(config_path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    LogErrorFmt("Failed to write config at %s", config_path.c_str());
    return false;
  }
  out << tbl;
  out.close();
  return true;
}

extern "C" void xemu_android_set_display_mode_setting(int mode);

static SetupFiles SyncSetupFiles() {
  SetupFiles out{};
  JNIEnv* env = GetEnv();
  jobject activity = GetActivity(env);
  if (!env || !activity) {
    LogError("JNI environment not ready for setup sync");
    return out;
  }

  LogInfo("SyncSetupFiles: start");
  const char* basePath = SDL_AndroidGetInternalStoragePath();
  int extState = SDL_AndroidGetExternalStorageState();
  if (extState & SDL_ANDROID_EXTERNAL_STORAGE_WRITE) {
    const char* external = SDL_AndroidGetExternalStoragePath();
    if (external && external[0] != '\0') {
      basePath = external;
    }
  }
  if (!basePath || basePath[0] == '\0') {
    LogError("Storage path not available");
    return out;
  }
  LogInfoFmt("SyncSetupFiles: base path %s", basePath);

  std::string base = std::string(basePath) + "/x1box";
  EnsureDirExists(base);
  out.eeprom = base + "/eeprom.bin";
  out.inline_aio_flag_path = base + "/inline_aio_required.flag";

  const std::string mcpxPath = GetPrefString(env, activity, "mcpxPath");
  const std::string flashPath = GetPrefString(env, activity, "flashPath");
  const std::string hddPath = GetPrefString(env, activity, "hddPath");
  const std::string dvdPath = GetPrefString(env, activity, "dvdPath");
  const std::string mcpxUri = GetPrefString(env, activity, "mcpxUri");
  const std::string flashUri = GetPrefString(env, activity, "flashUri");
  const std::string hddUri = GetPrefString(env, activity, "hddUri");
  const std::string dvdUri = GetPrefString(env, activity, "dvdUri");

  LogInfoFmt("Prefs mcpxPath=%s", mcpxPath.c_str());
  LogInfoFmt("Prefs flashPath=%s", flashPath.c_str());
  LogInfoFmt("Prefs hddPath=%s", hddPath.c_str());
  LogInfoFmt("Prefs dvdPath=%s", dvdPath.c_str());
  LogInfoFmt("Prefs mcpxUri=%s", mcpxUri.c_str());
  LogInfoFmt("Prefs flashUri=%s", flashUri.c_str());
  LogInfoFmt("Prefs hddUri=%s", hddUri.c_str());
  LogInfoFmt("Prefs dvdUri=%s", dvdUri.c_str());

  if (!mcpxPath.empty() && FileExists(mcpxPath)) {
    out.mcpx = mcpxPath;
  }
  if (out.mcpx.empty() && !mcpxUri.empty()) {
    out.mcpx = base + "/mcpx.bin";
    if (CopyUriToPath(env, activity, mcpxUri, out.mcpx)) {
      LogInfo("MCPX ROM synced to app storage");
    } else {
      LogError("Failed to sync MCPX ROM");
    }
  }
  if (!flashPath.empty() && FileExists(flashPath)) {
    out.flash = flashPath;
  }
  if (out.flash.empty() && !flashUri.empty()) {
    out.flash = base + "/flash.bin";
    if (CopyUriToPath(env, activity, flashUri, out.flash)) {
      LogInfo("Flash ROM synced to app storage");
    } else {
      LogError("Failed to sync flash ROM");
    }
  }
  if (!hddPath.empty() && FileExists(hddPath)) {
    out.hdd = hddPath;
  }
  if (out.hdd.empty() && !hddUri.empty()) {
    out.hdd = base + "/hdd.img";
    if (CopyUriToPath(env, activity, hddUri, out.hdd)) {
      LogInfo("HDD image synced to app storage");
    } else {
      LogError("Failed to sync HDD image");
    }
  }

  if (!dvdPath.empty() && FileExists(dvdPath)) {
    out.dvd = dvdPath;
  }
  if (out.dvd.empty() && !dvdUri.empty()) {
    out.dvd = OpenUriAsReadOnlyFdPath(env, activity, dvdUri);
    if (!out.dvd.empty()) {
      LogInfoFmt("DVD image opened directly from SAF fd: %s", out.dvd.c_str());
    } else {
      out.dvd = base + "/dvd.iso";
      if (CopyUriToPath(env, activity, dvdUri, out.dvd)) {
        LogInfo("DVD image synced to app storage");
      } else {
        LogError("Failed to sync DVD image");
      }
    }
  }

  EmulatorSettings emuSettings;
  emuSettings.surface_scale  = GetPrefInt(env, activity, "setting_surface_scale", 1);
  emuSettings.frame_rate_limit =
      GetPrefInt(env, activity, "setting_frame_rate_limit", 60);
  if (emuSettings.frame_rate_limit != 30 &&
      emuSettings.frame_rate_limit != 60) {
    emuSettings.frame_rate_limit = 60;
  }
  emuSettings.system_memory_mib =
      GetPrefInt(env, activity, "setting_system_memory_mib", 64);
  if (emuSettings.system_memory_mib != 64 &&
      emuSettings.system_memory_mib != 128) {
    emuSettings.system_memory_mib = 64;
  }
  emuSettings.use_dsp        = GetPrefBool(env, activity, "setting_use_dsp", false);
  emuSettings.hrtf           = GetPrefBool(env, activity, "setting_hrtf", true);
  emuSettings.cache_shaders  = GetPrefBool(env, activity, "setting_cache_shaders", true);
  emuSettings.hard_fpu       = GetPrefBool(env, activity, "setting_hard_fpu", true);
  emuSettings.skip_boot_anim =
      GetPrefBool(env, activity, "setting_skip_boot_anim", false);
  {
    std::string tcgThread = GetPrefString(env, activity, "setting_tcg_thread");
    if (tcgThread == "single") {
      emuSettings.tcg_thread = "single";
    }
  }
  {
    std::string rendererPref = GetPrefString(env, activity, "setting_renderer");
    if (rendererPref == "opengl") {
      emuSettings.renderer = "opengl";
    }
  }
  {
    std::string filteringPref = GetPrefString(env, activity, "setting_filtering");
    if (filteringPref == "nearest") {
      emuSettings.filtering = "nearest";
    }
  }
  emuSettings.vsync = GetPrefBool(env, activity, "setting_vsync", true);
  out.audio_driver = GetPrefString(env, activity, "setting_audio_driver");
  {
    std::string normalized = ToLowerAscii(out.audio_driver);
    if (normalized == "android" || normalized == "audiotrack") {
      out.audio_driver = "openslES";
    }
  }

  int displayMode = GetPrefInt(env, activity, "setting_display_mode", 0);
  xemu_android_set_display_mode_setting(displayMode);

  const std::string vulkanDriverUri = GetPrefString(env, activity, "setting_vulkan_driver_uri");
  if (!vulkanDriverUri.empty()) {
    std::string driverPath = base + "/vulkan_driver.so";
    if (CopyUriToPath(env, activity, vulkanDriverUri, driverPath)) {
      chmod(driverPath.c_str(), 0755);
      setenv("XEMU_VULKAN_DRIVER", driverPath.c_str(), 1);
      LogInfoFmt("Custom Vulkan driver staged: %s", driverPath.c_str());
    } else {
      LogError("Failed to copy custom Vulkan driver; using system default");
    }
  }

  out.config_path = base + "/xemu.toml";
  WriteConfigToml(out.config_path, out.mcpx, out.flash, out.hdd, out.dvd, out.eeprom, emuSettings);
  LogInfoFmt("SyncSetupFiles: config %s", out.config_path.c_str());
  LogInfoFmt("Resolved mcpx=%s", out.mcpx.c_str());
  LogInfoFmt("Resolved flash=%s", out.flash.c_str());
  LogInfoFmt("Resolved hdd=%s", out.hdd.c_str());
  LogInfoFmt("Resolved dvd=%s", out.dvd.c_str());
  LogInfoFmt("Resolved eeprom=%s", out.eeprom.c_str());
  return out;
}
}

extern "C" int xemu_android_main(int argc, char** argv);
extern "C" void qemu_init(int argc, char** argv);
extern "C" int (*qemu_main)(void);
extern "C" void xemu_android_display_preinit(void);
extern "C" void xemu_android_display_wait_ready(void);
extern "C" void xemu_android_display_loop(void);
extern "C" void xemu_android_set_inline_aio_crash_flag_path(const char* path);

struct QemuLaunchContext {
  int argc;
  char** argv;
};

static int SDLCALL QemuThreadMain(void* data) {
  auto* ctx = static_cast<QemuLaunchContext*>(data);
  LogInfoInt("QemuThreadMain: show_welcome=%d", g_config.general.show_welcome ? 1 : 0);
  LogInfoFmt("QemuThreadMain: bootrom=%s", g_config.sys.files.bootrom_path ? g_config.sys.files.bootrom_path : "(null)");
  LogInfo("QemuThreadMain: starting");
  return xemu_android_main(ctx->argc, ctx->argv);
}

extern "C" int xemu_android_main(int argc, char** argv) {
  if (!qemu_main) {
    LogError("xemu core not linked; qemu_main missing");
    return 1;
  }
  LogInfo("xemu_android_main: qemu_init");
  qemu_init(argc, argv);
  LogInfo("xemu_android_main: qemu_main");
  int rc = qemu_main();
  LogErrorInt("xemu_android_main: qemu_main returned %d", rc);
  return rc;
}

extern "C" int SDL_main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  LogInfo("SDL_main: start");
  std::string audio_driver_hint = ResolveAndroidAudioDriverHint();
  SDL_SetHintWithPriority(SDL_HINT_AUDIODRIVER, audio_driver_hint.c_str(),
                          SDL_HINT_OVERRIDE);
  LogInfoFmt("SDL_HINT_AUDIODRIVER=%s", audio_driver_hint.c_str());
  SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
  SDL_DisableScreenSaver();

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "SDL_Init failed: %s", SDL_GetError());
    return 1;
  }
  SDL_GameControllerEventState(SDL_ENABLE);
  LoadGameControllerMappingsFromAssets();

  SetupFiles setup = SyncSetupFiles();

  // If the user explicitly chose an audio driver in Settings, override the
  // hint we set at startup.  Audio is not initialized until qemu_init() so
  // it is safe to change SDL_HINT_AUDIODRIVER here.
  if (!setup.audio_driver.empty()) {
    // Reuse ResolveAndroidAudioDriverHint logic by temporarily writing the
    // pref value into XEMU_ANDROID_AUDIO_DRIVER and re-resolving.
    setenv("XEMU_ANDROID_AUDIO_DRIVER", setup.audio_driver.c_str(), 1);
    std::string resolved = ResolveAndroidAudioDriverHint();
    SDL_SetHintWithPriority(SDL_HINT_AUDIODRIVER, resolved.c_str(),
                            SDL_HINT_OVERRIDE);
    LogInfoFmt("SDL_HINT_AUDIODRIVER (from prefs)=%s", resolved.c_str());
  }

  xemu_android_set_inline_aio_crash_flag_path(setup.inline_aio_flag_path.empty()
                                                   ? nullptr
                                                   : setup.inline_aio_flag_path.c_str());

  if (!SDL_getenv("XEMU_ANDROID_INLINE_AIO")) {
    const bool use_inline_aio =
        ShouldEnableInlineAioWorkaround(setup.inline_aio_flag_path);
    setenv("XEMU_ANDROID_INLINE_AIO", use_inline_aio ? "1" : "0", 1);
    LogInfoFmt("XEMU_ANDROID_INLINE_AIO=%s", use_inline_aio ? "1" : "0");
  }

  if (!setup.config_path.empty()) {
    LogInfo("SDL_main: loading config");
    xemu_settings_set_path(setup.config_path.c_str());
    if (!xemu_settings_load()) {
      const char* err = xemu_settings_get_error_message();
      if (!err) {
        err = "Failed to load config file";
      }
      LogError(err);
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                               "Failed to load xemu config file",
                               err,
                               nullptr);
      SDL_Quit();
      return 1;
    }
    LogInfo("SDL_main: config loaded");
    LogInfoInt("Config show_welcome=%d", g_config.general.show_welcome ? 1 : 0);
    LogInfoFmt("Config bootrom=%s", g_config.sys.files.bootrom_path ? g_config.sys.files.bootrom_path : "(null)");
    LogInfoFmt("Config flashrom=%s", g_config.sys.files.flashrom_path ? g_config.sys.files.flashrom_path : "(null)");
    LogInfoFmt("Config hdd=%s", g_config.sys.files.hdd_path ? g_config.sys.files.hdd_path : "(null)");
    LogInfoFmt("Config dvd=%s", g_config.sys.files.dvd_path ? g_config.sys.files.dvd_path : "(null)");
    LogInfoFmt("Config eeprom=%s", g_config.sys.files.eeprom_path ? g_config.sys.files.eeprom_path : "(null)");

    // Ensure config strings are non-null and aligned with Android setup paths.
    if (!setup.mcpx.empty()) {
      xemu_settings_set_string(&g_config.sys.files.bootrom_path, setup.mcpx.c_str());
    } else if (!g_config.sys.files.bootrom_path) {
      xemu_settings_set_string(&g_config.sys.files.bootrom_path, "");
    }
    if (!setup.flash.empty()) {
      xemu_settings_set_string(&g_config.sys.files.flashrom_path, setup.flash.c_str());
    } else if (!g_config.sys.files.flashrom_path) {
      xemu_settings_set_string(&g_config.sys.files.flashrom_path, "");
    }
    if (!setup.hdd.empty()) {
      xemu_settings_set_string(&g_config.sys.files.hdd_path, setup.hdd.c_str());
    } else if (!g_config.sys.files.hdd_path) {
      xemu_settings_set_string(&g_config.sys.files.hdd_path, "");
    }
    if (!setup.dvd.empty()) {
      xemu_settings_set_string(&g_config.sys.files.dvd_path, setup.dvd.c_str());
    } else if (!g_config.sys.files.dvd_path) {
      xemu_settings_set_string(&g_config.sys.files.dvd_path, "");
    }
    if (!setup.eeprom.empty()) {
      xemu_settings_set_string(&g_config.sys.files.eeprom_path, setup.eeprom.c_str());
    } else if (!g_config.sys.files.eeprom_path) {
      xemu_settings_set_string(&g_config.sys.files.eeprom_path, "");
    }
    g_config.general.show_welcome = false;
    g_config.perf.cache_shaders = true;
    LogInfoInt("Config final show_welcome=%d", g_config.general.show_welcome ? 1 : 0);
    LogInfoInt("Config final cache_shaders=%d", g_config.perf.cache_shaders ? 1 : 0);
    LogInfoFmt("Config final renderer=%s",
               (g_config.display.renderer == 0) ? "Vulkan" :
               (g_config.display.renderer == 1) ? "OpenGL" : "Null");
    LogInfoFmt("Config final bootrom=%s", g_config.sys.files.bootrom_path ? g_config.sys.files.bootrom_path : "(null)");
    LogInfoFmt("Config final flashrom=%s", g_config.sys.files.flashrom_path ? g_config.sys.files.flashrom_path : "(null)");
    LogInfoFmt("Config final hdd=%s", g_config.sys.files.hdd_path ? g_config.sys.files.hdd_path : "(null)");
    LogInfoFmt("Config final dvd=%s", g_config.sys.files.dvd_path ? g_config.sys.files.dvd_path : "(null)");
    LogInfoFmt("Config final eeprom=%s", g_config.sys.files.eeprom_path ? g_config.sys.files.eeprom_path : "(null)");

    std::vector<std::string> arg_storage;
    arg_storage.emplace_back("xemu");
    if (IsTcgTuningEnabled()) {
      const char* tcg_thread = GetTcgThreadFromEnv();
      int tcg_tb_size = GetTcgTbSizeFromEnv();
      char accel_opts[64];
      snprintf(accel_opts, sizeof(accel_opts), "tcg,thread=%s,tb-size=%d",
               tcg_thread, tcg_tb_size);
      arg_storage.emplace_back("-accel");
      arg_storage.emplace_back(accel_opts);
      LogInfoFmt("SDL_main: using accel %s", accel_opts);
    } else {
      LogInfo("SDL_main: TCG tuning disabled");
    }

    std::vector<char*> xemu_argv;
    xemu_argv.reserve(arg_storage.size() + 1);
    for (auto& arg : arg_storage) {
      xemu_argv.push_back(const_cast<char*>(arg.c_str()));
    }
    xemu_argv.push_back(nullptr);
    LogInfo("SDL_main: launching xemu core");
    xemu_android_display_preinit();

    QemuLaunchContext launch_ctx{
      static_cast<int>(arg_storage.size()),
      xemu_argv.data(),
    };
    SDL_Thread* qemu_thread = SDL_CreateThread(QemuThreadMain, "qemu_main", &launch_ctx);
    if (!qemu_thread) {
      LogErrorFmt("Failed to start xemu thread: %s", SDL_GetError());
      return 1;
    }
    LogInfo("SDL_main: qemu thread started");
    (void)qemu_thread;
    xemu_android_display_wait_ready();
    LogInfo("SDL_main: display ready, entering render loop");
    xemu_android_display_loop();
    // Force process exit so Android cannot freeze-and-reuse this process for a
    // subsequent game launch.  qemu_init() cannot be called twice in the same
    // process; if the process is thawed and SDL_main is re-entered, qemu_init
    // asserts/aborts (confirmed by Burnout 3 logcat: SIGABRT in tid 29278).
    _exit(0);
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

  SDL_Window* window = SDL_CreateWindow(
    "xemu (Android bootstrap)",
    SDL_WINDOWPOS_CENTERED,
    SDL_WINDOWPOS_CENTERED,
    1280,
    720,
    SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN
  );

  if (!window) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "SDL_CreateWindow failed: %s", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  SDL_GLContext gl = SDL_GL_CreateContext(window);
  if (!gl) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "SDL_GL_CreateContext failed: %s", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  SDL_GL_MakeCurrent(window, gl);
  SDL_GL_SetSwapInterval(1);

  LogInfo("xemu Android bootstrap running (core not wired yet)");

  bool running = true;
  while (running) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT) {
        running = false;
      } else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_AC_BACK) {
        running = false;
      }
    }

    int w = 0;
    int h = 0;
    SDL_GL_GetDrawableSize(window, &w, &h);
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    glViewport(0, 0, w, h);
    glClearColor(0.05f, 0.07f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    SDL_GL_SwapWindow(window);
  }

  SDL_GL_DeleteContext(gl);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
