plugins { kotlin("jvm") }

// The engine from Kotlin, end to end on a real device with no display. A scene from the engine's library, scripts
// loaded from compiled classes, run, and swapped for an edited build while they run. One JVM: an application is one
// per process.

// The scripts under test, compiled on their own: as first written, as edited, and with a property no script can keep.
val scriptsA = sourceSets.create("scriptsA")
val scriptsB = sourceSets.create("scriptsB")
val scriptsBad = sourceSets.create("scriptsBad")

// E6's gate: the light's scripts as first written, and as edited four times while the scene runs.
val shadowBuilds = listOf("A", "B", "C", "D", "E").map { sourceSets.create("shadow$it") }

dependencies {
    testImplementation(project(":koral-engine"))
    testImplementation(kotlin("test"))
    testImplementation("org.junit.jupiter:junit-jupiter:5.13.4")
    testRuntimeOnly("org.junit.platform:junit-platform-launcher")
    "scriptsAImplementation"(project(":koral-engine"))
    "scriptsBImplementation"(project(":koral-engine"))
    "scriptsBadImplementation"(project(":koral-engine"))
    shadowBuilds.forEach { "${it.name}Implementation"(project(":koral-engine")) }
}

tasks.test {
    useJUnitPlatform()
    forkEvery = 1
    dependsOn(scriptsA.classesTaskName, scriptsB.classesTaskName, scriptsBad.classesTaskName)
    systemProperty("koral.engine.test.a", scriptsA.output.classesDirs.asPath)
    systemProperty("koral.engine.test.b", scriptsB.output.classesDirs.asPath)
    systemProperty("koral.engine.test.bad", scriptsBad.output.classesDirs.asPath)
    shadowBuilds.forEach { dependsOn(it.classesTaskName); systemProperty("koral.engine.test.${it.name}", it.output.classesDirs.asPath) }
    (findProperty("shadowProject") as String?)?.let { systemProperty("koral.engine.test.shadowProject", it) }
    (findProperty("engineLibrary") as String?)?.let { systemProperty("koral.engine.library", it) }
    // Koral's own library is found from its SDK, the one the build was pointed at.
    ((findProperty("koralSdk") as String?) ?: System.getenv("KORAL_SDK"))?.let { environment("KORAL_SDK", it) }
    outputs.upToDateWhen { false }
    testLogging {
        events("passed", "failed", "skipped")
        showStandardStreams = System.getenv("KORAL_TEST_OUTPUT") != null
        exceptionFormat = org.gradle.api.tasks.testing.logging.TestExceptionFormat.FULL
    }
}
