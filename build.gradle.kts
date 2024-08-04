plugins {
    java
    application
}

group = "com.tkr"
version = "1.0.0"

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(17))
    }
}

repositories {
    mavenCentral()
}

dependencies {
    testImplementation("org.junit.jupiter:junit-jupiter:5.10.2")
}

application {
    mainClass.set("com.tkr.tools.TkrCtl")
}

tasks.test {
    useJUnitPlatform()
}

tasks.jar {
    manifest {
        attributes["Main-Class"] = "com.tkr.tools.TkrCtl"
    }
    duplicatesStrategy = DuplicatesStrategy.EXCLUDE
    from(sourceSets.main.get().output)
}

tasks.register<Jar>("fuzzJar") {
    archiveClassifier.set("fuzz")
    from(sourceSets.main.get().output)
    destinationDirectory.set(layout.buildDirectory.dir("libs"))
}

sourceSets {
    create("fuzz") {
        java.srcDir("src/fuzz/java")
        compileClasspath += sourceSets.main.get().output
        runtimeClasspath += sourceSets.main.get().output
    }
}

tasks.register<JavaCompile>("compileFuzz") {
    source = sourceSets["fuzz"].java
    classpath = sourceSets["fuzz"].compileClasspath
    destinationDirectory.set(layout.buildDirectory.dir("fuzz-classes"))
    options.release.set(17)
}
