import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.util.Properties
import java.util.zip.ZipInputStream

plugins {
    id("com.android.application")
    id("io.sentry.android.gradle") version "4.14.0"
}

// Sentry DSN is not committed. Supply it via the SENTRY_DSN environment
// variable or a `sentry.dsn=` line in local.properties (both untracked).
// When unset, the DSN resolves to "" and the Sentry SDK stays disabled.
val sentryDsn: String = providers.environmentVariable("SENTRY_DSN").orNull
    ?: rootProject.file("local.properties").let { f ->
        if (f.exists()) Properties().apply { f.inputStream().use { load(it) } }
            .getProperty("sentry.dsn", "")
        else ""
    }

// ---------------------------------------------------------------------------
// Download SDL3 Java sources (SDLActivity etc.) for the Android build
// ---------------------------------------------------------------------------
val sdlVersion = "release-3.4.14"
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

        // Inject the DSN into AndroidManifest's io.sentry.dsn meta-data.
        manifestPlaceholders["sentryDsn"] = sentryDsn

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
                // Share the same DSN with the native (sentry-android-ndk) build.
                if (sentryDsn.isNotEmpty()) arguments += "-DSENTRY_DSN=$sentryDsn"
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
// Copy game data files into assets/data/. At startup the native side copies
// the data/ and Brains/ asset trees into the app's files directory and makes
// it the working directory (main_android.c), so the shared code's relative
// data/... opens and directory listings work as they do on the desktop and
// in the iOS bundle. The same files the iOS bundle carries (the IOS_DATA_*
// lists in the root CMakeLists.txt); the country database stays out.
// ---------------------------------------------------------------------------
val dataDir = file("../../data")
val assetsRootDir = file("src/main/assets")
val assetsDataDir = file("src/main/assets/data")

tasks.register<Copy>("copyGameAssets") {
    from(dataDir) {
        include("*.bmp")
        include("*.png")
        include("bot_names.json")
        include("fonts/*.ttf")
        include("fonts/*.otf")
        include("sounds/*.wav")
        include("maps/*.map")
        include("maps/*.lua")
        include("mods/*.lua")
        include("flags/*.svg")
        include("svg/*.png")
        include("svg/*.svg")
        include("ui/*.svg")
        include("ui/ping/*.svg")
        include("icons/*.png")
        exclude("dbip-country-lite.mmdb")
    }
    into(assetsDataDir)
}

// Copy brain scripts into assets/Brains/, the directory name the brain
// scanner and the menu's background game look in (LUA_BRAINS_DIR), as the
// iOS bundle spells it. The opt/ build of GoalHunter comes with the glob.
val brainsDir = file("../../brains")
val assetsBrainsDir = file("src/main/assets/Brains")

tasks.register<Copy>("copyBrainAssets") {
    // An earlier build wrote this tree as assets/brains. On a case-insensitive
    // host filesystem the copy below would land inside that directory and the
    // APK would keep the old spelling, which the extracted tree must not, so
    // the directory is renamed first; File.exists() cannot tell the two apart,
    // the parent's listing can.
    doFirst {
        val parent = assetsBrainsDir.parentFile
        val stale = parent.list()?.firstOrNull { it == "brains" }
        if (stale != null) {
            File(parent, stale).renameTo(assetsBrainsDir)
        }
    }
    from(brainsDir) {
        include("GoalHunter/**/*.lua")
    }
    into(assetsBrainsDir)
}

// The listing the native side extracts from: asset directories cannot be
// listed through SDL. One relative path per line after a stamp line, a hash
// of every listed file's path and bytes, so a build whose assets changed
// refreshes the extracted copy and one whose assets did not leaves it alone.
tasks.register("writeAssetManifest") {
    dependsOn("copyGameAssets", "copyBrainAssets", "copyLangAssets", "copyLicenceAssets")
    val manifest = File(assetsRootDir, "asset_manifest.txt")
    inputs.files(fileTree(assetsDataDir), fileTree(assetsBrainsDir))
    outputs.file(manifest)
    doLast {
        val digest = MessageDigest.getInstance("SHA-1")
        val paths = mutableListOf<String>()
        for (top in listOf("data", "Brains")) {
            val dir = File(assetsRootDir, top)
            if (!dir.isDirectory) continue
            dir.walkTopDown().filter { it.isFile }.forEach { f ->
                paths += top + "/" + f.relativeTo(dir).invariantSeparatorsPath
            }
        }
        paths.sort()
        for (p in paths) {
            digest.update(p.toByteArray())
            digest.update(File(assetsRootDir, p).readBytes())
        }
        val stamp = digest.digest().joinToString("") { "%02x".format(it) }
        manifest.writeText("stamp $stamp\n" + paths.joinToString("\n") + "\n")
    }
}

// Copy language files into assets/data/lang/ so the runtime picker can find them
val langDir = file("../../data/lang")
val assetsLangDir = file("src/main/assets/data/lang")

tasks.register<Copy>("copyLangAssets") {
    from(langDir) {
        include("*.txt")
    }
    into(assetsLangDir)
}

// Copy the licence, its additional permissions and the About-box docs into
// assets/data/. The GPL requires every copy of the program to carry a copy
// of the licence.
val repoRootDir = file("../..")

tasks.register<Copy>("copyLicenceAssets") {
    from(repoRootDir) {
        include("LICENSE")
        include("LICENSE-EXCEPTION.md")
        include("THIRD_PARTY_NOTICES.md")
        include("AUTHORS.md")
    }
    into(assetsDataDir)
}

tasks.configureEach {
    if (name == "preBuild") {
        dependsOn("copyGameAssets")
        dependsOn("copyBrainAssets")
        dependsOn("copyLangAssets")
        dependsOn("copyLicenceAssets")
        dependsOn("writeAssetManifest")
    }
}
