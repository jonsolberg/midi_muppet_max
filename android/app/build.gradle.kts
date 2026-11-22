plugins {
    id("com.android.application")
}

// The app's version is the project's VERSION file (also the GitHub release tag).
val versionText = rootProject.file("../VERSION").readText().trim()
val versionParts = versionText.split(".").map { it.toIntOrNull() ?: 0 } + listOf(0, 0, 0)

// Release signing: the key comes from GitHub secrets (see .github/workflows/release.yml).
val keystorePath: String? = System.getenv("SIGNING_KEYSTORE")

// The editor page is the same file as the web editor (editor/index.html), copied in at build time
// with the app's USB bridge (assets/android-usb.js) loaded before anything else.
val editorAssets = layout.buildDirectory.dir("generated/editorAssets")
val copyEditor by tasks.registering(Copy::class) {
    from(rootProject.file("../editor/index.html"))
    into(editorAssets)
    filter { line: String -> line.replace("<head>", "<head>\n<script src=\"android-usb.js\"></script>") }
}

android {
    namespace = "se.jonsolberg.midimuppetmax"
    compileSdk = 35

    defaultConfig {
        applicationId = "se.jonsolberg.midimuppetmax"
        minSdk = 26
        targetSdk = 35
        versionCode = versionParts[0] * 10000 + versionParts[1] * 100 + versionParts[2]
        versionName = versionText
    }

    buildFeatures {
        buildConfig = true
    }

    sourceSets["main"].assets.srcDir(editorAssets)

    signingConfigs {
        create("release") {
            if (keystorePath != null) {
                storeFile = file(keystorePath)
                storePassword = System.getenv("SIGNING_STORE_PASSWORD")
                keyAlias = System.getenv("SIGNING_KEY_ALIAS")
                keyPassword = System.getenv("SIGNING_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = if (keystorePath != null) signingConfigs.getByName("release")
            else signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

tasks.named("preBuild") { dependsOn(copyEditor) }
