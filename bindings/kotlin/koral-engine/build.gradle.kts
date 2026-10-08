plugins { kotlin("jvm") }

// The engine from Kotlin: a scene's entities and graph, scripts as classes, and the host that loads them again.
dependencies {
    implementation(kotlin("stdlib"))
    api("koral:koral:0.1.0")
}
