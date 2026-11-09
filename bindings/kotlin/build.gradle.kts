plugins {
    kotlin("jvm") version "2.4.20" apply false
}

subprojects {
    plugins.withId("org.jetbrains.kotlin.jvm") {
        extensions.configure<org.jetbrains.kotlin.gradle.dsl.KotlinJvmProjectExtension> {
            // Arguments by their names: what a script's condition's inputs are called on the node.
            compilerOptions { jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_25); javaParameters.set(true) }
        }
        extensions.configure<JavaPluginExtension> {
            sourceCompatibility = JavaVersion.VERSION_25
            targetCompatibility = JavaVersion.VERSION_25
        }
    }
    tasks.withType<JavaExec>().configureEach { jvmArgs("--enable-native-access=ALL-UNNAMED") }
    tasks.withType<Test>().configureEach { jvmArgs("--enable-native-access=ALL-UNNAMED") }
}

// The library, as a Maven artifact a project's scripts depend on: -PengineRepo=<dir> publishes it there.
val engineRepo = findProperty("engineRepo") as String?
project(":koral-engine") {
    apply(plugin = "maven-publish")
    group = "koral"
    version = "0.1.0"
    plugins.withId("org.jetbrains.kotlin.jvm") {
        extensions.configure<JavaPluginExtension> { withSourcesJar() }
        extensions.configure<PublishingExtension> {
            publications { create<MavenPublication>("library") { from(components["java"]) } }
            if (engineRepo != null) repositories { maven { name = "engine"; url = uri(engineRepo) } }
        }
    }
}
