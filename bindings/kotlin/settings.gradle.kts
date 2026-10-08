// Koral Engine from Kotlin: scripts as classes, over koral_engine.h through java.lang.foreign.
// The Koral SDK's Maven repository is where Koral's own Kotlin libraries come from: -PkoralSdk=<sdk>, or KORAL_SDK.
pluginManagement {
    repositories { gradlePluginPortal(); mavenCentral() }
}
val koralSdk = (providers.gradleProperty("koralSdk").orNull ?: System.getenv("KORAL_SDK"))
    ?: error("Where is the Koral SDK? Pass -PkoralSdk=<sdk>, or set KORAL_SDK.")
dependencyResolutionManagement {
    repositories {
        maven(url = uri("$koralSdk/share/Koral/maven"))
        mavenCentral()
        google()
    }
}
rootProject.name = "koral-engine-kotlin"
include("koral-engine", "tests")
