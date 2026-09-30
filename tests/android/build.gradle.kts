plugins {
    id("com.android.application") version "9.3.2" apply false
    // AGP compiles Kotlin itself; this adds the Compose compiler, whose
    // version is the Kotlin version.
    id("org.jetbrains.kotlin.plugin.compose") version "2.4.20" apply false
}
