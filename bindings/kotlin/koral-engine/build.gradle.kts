plugins { kotlin("jvm") }

// The engine from Kotlin: a scene's entities and graph, scripts as classes, and the host that loads them again.
dependencies {
    implementation(kotlin("stdlib"))
    api("koral:koral:0.1.0")
    api("koral:koral-ui:0.1.0")     // a project's scripts have all of Koral: its UI and its networking too
    api("koral:koral-net:0.1.0")
}

// What the engine's programs start a JVM with, inside their own process, to run a project's Kotlin scripts:
// -PhostDir=<dir> lays it out there. lib/ is this library and everything it runs on — the JVM's classpath, which the
// scripts are compiled against — and compiler/ is Kotlin's compiler, which compiles them in that same process.
val kotlinCompiler by configurations.creating
dependencies {
    kotlinCompiler("org.jetbrains.kotlin:kotlin-compiler-embeddable:${org.jetbrains.kotlin.gradle.plugin.getKotlinPluginVersion(logger)}")
}
tasks.register("hostLayout") {
    dependsOn(tasks.jar)
    val root = (findProperty("hostDir") as String?)?.let { file(it) } ?: layout.buildDirectory.dir("host").get().asFile
    val runtime = configurations.runtimeClasspath.get()
    val jar = tasks.jar.get().archiveFile
    doLast {
        delete(root)
        // Each jar by its group as well as its name: two of them are runtime-desktop-1.12.1.jar, JetBrains' and AndroidX's.
        fun lay(into: File, configuration: Configuration) {
            into.mkdirs()
            for (artifact in configuration.incoming.artifacts.artifacts) {
                val id = artifact.id.componentIdentifier
                val name = if (id is org.gradle.api.artifacts.component.ModuleComponentIdentifier) "${id.group}.${artifact.file.name}" else artifact.file.name
                artifact.file.copyTo(into.resolve(name), overwrite = true)
            }
        }
        lay(root.resolve("lib"), runtime)
        jar.get().asFile.copyTo(root.resolve("lib/koral-engine.jar"), overwrite = true)
        lay(root.resolve("compiler"), kotlinCompiler)
    }
}
