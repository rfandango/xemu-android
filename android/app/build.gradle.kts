import java.util.Properties
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
  id("com.android.application")
  id("org.jetbrains.kotlin.android")
}

val keystorePropertiesFile = rootProject.file("key.properties")
val keystoreProperties = Properties()
val hasKeystoreProperties = keystorePropertiesFile.exists()

if (hasKeystoreProperties) {
  keystorePropertiesFile.inputStream().use { keystoreProperties.load(it) }
}

val hasReleaseKeystore = hasKeystoreProperties &&
  listOf("storeFile", "storePassword", "keyAlias", "keyPassword").all {
    !keystoreProperties.getProperty(it).isNullOrBlank()
  }

android {
  namespace = "com.izzy2lost.x1box"
  compileSdk = 36
  buildToolsVersion = "36.1.0"
  ndkVersion = "29.0.14206865"

  defaultConfig {
    applicationId = "com.izzy2lost.x1box"
    minSdk = 26
    targetSdk = 36

    versionCode = 13
    versionName = "1.1.2"

    ndk {
      abiFilters += listOf("arm64-v8a")
    }

    externalNativeBuild {
      cmake {
        arguments += listOf(
          "-DXEMU_ANDROID_BUILD_ID=3",
          "-DXEMU_ENABLE_XISO_CONVERTER=ON",
          "-DCMAKE_C_FLAGS_DEBUG=-O2 -g0",
          "-DCMAKE_CXX_FLAGS_DEBUG=-O2 -g0",
          "-DCMAKE_C_FLAGS_RELEASE=-O3 -g0 -march=armv8.2-a -ffunction-sections -fdata-sections",
          "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -g0 -march=armv8.2-a -ffunction-sections -fdata-sections",
          "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=-Wl,--gc-sections",
          "-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=-Wl,--gc-sections"
        )
        cppFlags += listOf("-std=c++17", "-fexceptions", "-frtti")
      }
    }
  }

  signingConfigs {
    if (hasReleaseKeystore) {
      create("release") {
        storeFile = file(keystoreProperties.getProperty("storeFile"))
        storePassword = keystoreProperties.getProperty("storePassword")
        keyAlias = keystoreProperties.getProperty("keyAlias")
        keyPassword = keystoreProperties.getProperty("keyPassword")
      }
    }
  }

  buildTypes {
    debug {
      ndk {
        debugSymbolLevel = "NONE"
      }
    }
    release {
      isMinifyEnabled = false
      proguardFiles(
        getDefaultProguardFile("proguard-android-optimize.txt"),
        "proguard-rules.pro"
      )
      if (hasReleaseKeystore) {
        signingConfig = signingConfigs.getByName("release")
      }
    }
  }

  externalNativeBuild {
    cmake {
      path = file("src/main/cpp/CMakeLists.txt")
      version = "3.30.3"
    }
  }

  packaging {
    resources.excludes += setOf(
      "**/*.md",
      "META-INF/LICENSE*",
      "META-INF/NOTICE*"
    )
  }

  compileOptions {
    sourceCompatibility = JavaVersion.VERSION_21
    targetCompatibility = JavaVersion.VERSION_21
  }

}

dependencies {
  implementation("androidx.core:core-ktx:1.15.0")
  implementation("androidx.appcompat:appcompat:1.7.0")
  implementation("androidx.constraintlayout:constraintlayout:2.1.4")
  implementation("androidx.documentfile:documentfile:1.0.1")
  implementation("io.coil-kt:coil:2.7.0")
  implementation("com.google.android.material:material:1.14.0-alpha07")
}

kotlin {
  compilerOptions {
    jvmTarget.set(JvmTarget.JVM_21)
  }
}
