import java.net.HttpURLConnection
import java.net.URL
import java.util.zip.ZipInputStream

plugins {
    id("com.android.application")
    id("io.sentry.android.gradle") version "4.14.0"
}

// ---------------------------------------------------------------------------
// Download SDL3 Java sources (SDLActivity etc.) for the Android build
// ---------------------------------------------------------------------------
val sdlVersion = "release-3.4.2"
val sdlJavaExtractDir = layout.buildDirectory.dir("sdl3-java")

tasks.register("downloadSdlJava") {
    val outputDir = sdlJavaExtractDir.get().asFile
    val sdlDir = File(outputDir, "SDL-$sdlVersion")
    outputs.dir(sdlDir)
    doLast {
        // Use a marker file instead of directory existence (avoids stale empty dirs)
        val marker = File(outputDir, ".sdl3-done")
        if (!marker.exists()) {
            // Clean up any previous failed attempt
            if (sdlDir.exists()) sdlDir.deleteRecursively()
            outputDir.mkdirs()
            val zipFile = File(outputDir, "sdl3.zip")
            val url = "https://github.com/libsdl-org/SDL/archive/refs/tags/$sdlVersion.zip"
            // Download with redirect following
            var conn = URL(url).openConnection() as HttpURLConnection
            conn.instanceFollowRedirects = true
            // Manually follow redirects (Java doesn't follow HTTP→HTTPS)
            var redirects = 0
            while (conn.responseCode in 301..302 && redirects < 5) {
                val loc = conn.getHeaderField("Location")
                conn.disconnect()
                conn = URL(loc).openConnection() as HttpURLConnection
                conn.instanceFollowRedirects = true
                redirects++
            }
            conn.inputStream.use { inp ->
                zipFile.outputStream().use { out -> inp.copyTo(out) }
            }
            conn.disconnect()
            // Extract
            ZipInputStream(zipFile.inputStream()).use { zis ->
                var entry = zis.nextEntry
                while (entry != null) {
                    val dest = File(outputDir, entry.name)
                    if (entry.isDirectory) {
                        dest.mkdirs()
                    } else {
                        dest.parentFile.mkdirs()
                        dest.outputStream().use { out -> zis.copyTo(out) }
                    }
                    entry = zis.nextEntry
                }
            }
            zipFile.delete()
            marker.writeText("ok")
        }
    }
}

tasks.configureEach {
    if (name.startsWith("compile") && name.endsWith("JavaWithJavac")) {
        dependsOn("downloadSdlJava")
    }
}

android {
    namespace = "com.winbolo.android"
    compileSdk = 34
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "com.winbolo.android"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"

        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DCMAKE_POLICY_VERSION_MINIMUM=3.10",
                    "-Wno-deprecated"
                )
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("CMakeLists.txt")
            version = "3.31.0+"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    // Include SDL3's Java sources (SDLActivity, etc.)
    sourceSets["main"].java.srcDir(
        sdlJavaExtractDir.map { it.dir("SDL-$sdlVersion/android-project/app/src/main/java") }
    )

    // Point assets at the copied data directory
    sourceSets {
        getByName("main") {
            assets.srcDirs("src/main/assets")
        }
    }
}

dependencies {
    implementation("io.sentry:sentry-android:7.18.0")
    implementation("io.sentry:sentry-android-ndk:7.18.0")
}

// ---------------------------------------------------------------------------
// Copy game data files into assets/data/ so they are accessible at runtime
// via SDL_IOFromFile("data/tile.bmp", ...) etc.
// Excludes large/unneeded files (mmdb, button_*.bmp menu graphics).
// ---------------------------------------------------------------------------
val dataDir = file("../../data")
val assetsDataDir = file("src/main/assets/data")

tasks.register<Copy>("copyGameAssets") {
    from(dataDir) {
        include("*.bmp")
        include("*.png")
        include("*.ttf")
        include("sounds/*.wav")
        include("maps/*.map")
        include("flags/*.svg")
        include("svg/*.png")
        include("svg/*.svg")
        exclude("dbip-country-lite.mmdb")
    }
    into(assetsDataDir)
}

// Copy brain scripts into assets/brains/ for the background bot game
val brainsDir = file("../../brains")
val assetsBrainsDir = file("src/main/assets/brains")

tasks.register<Copy>("copyBrainAssets") {
    from(brainsDir) {
        include("NewAutopilot/**/*.lua")
    }
    into(assetsBrainsDir)
}

// Copy language files into assets/lang/ so the runtime picker can find them
val langDir = file("../../lang")
val assetsLangDir = file("src/main/assets/lang")

tasks.register<Copy>("copyLangAssets") {
    from(langDir) {
        include("*.txt")
    }
    into(assetsLangDir)
}

tasks.configureEach {
    if (name == "preBuild") {
        dependsOn("copyGameAssets")
        dependsOn("copyBrainAssets")
        dependsOn("copyLangAssets")
    }
}
