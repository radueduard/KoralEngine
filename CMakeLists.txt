cmake_minimum_required(VERSION 3.28)
project(KoralEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(KORAL_ENGINE_BUILD_TESTS "Build the engine's tests" ON)
# Address and undefined-behaviour sanitizers, for the tests: -DKORAL_ENGINE_SANITIZE=ON.
option(KORAL_ENGINE_SANITIZE "Build with ASan and UBSan" OFF)
if(KORAL_ENGINE_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

if(KORAL_ENGINE_BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    include(GoogleTest)
endif()

add_subdirectory(modules/kecs)
add_subdirectory(modules/kscript)

# What runs on the GPU needs the Koral framework: an SDK, which is a Koral checkout's `stage/debug` or
# `stage/release`. The one that matches this build is found by itself when the checkout is known —
# -DKORAL_FRAMEWORK_DIR=<checkout>, the KORAL_FRAMEWORK_DIR environment variable, or a checkout beside this one —
# so that a Debug build and a Release build each use their own. CMAKE_PREFIX_PATH or Koral_DIR, given, win.
# Without an SDK, only what stands alone is built.
set(KORAL_FRAMEWORK_DIR "" CACHE PATH "A checkout of the Koral framework, whose stage/<debug|release> is the SDK to build against")
if(NOT KORAL_FRAMEWORK_DIR)
    foreach(candidate "$ENV{KORAL_FRAMEWORK_DIR}" "${CMAKE_SOURCE_DIR}/../CLionProjects/GFX" "${CMAKE_SOURCE_DIR}/../GFX" "${CMAKE_SOURCE_DIR}/../Koral")
        if(candidate AND EXISTS "${candidate}/stage")
            get_filename_component(candidate "${candidate}" ABSOLUTE)
            set(KORAL_FRAMEWORK_DIR "${candidate}" CACHE PATH "A checkout of the Koral framework, whose stage/<debug|release> is the SDK to build against" FORCE)
            break()
        endif()
    endforeach()
endif()
if(KORAL_FRAMEWORK_DIR AND NOT Koral_DIR AND NOT CMAKE_PREFIX_PATH)
    # A build that is not Debug is one of the optimised ones: they all go with the framework's release SDK.
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(KORAL_ENGINE_SDK_KIND debug)
    else()
        set(KORAL_ENGINE_SDK_KIND release)
    endif()
    if(EXISTS "${KORAL_FRAMEWORK_DIR}/stage/${KORAL_ENGINE_SDK_KIND}/lib/cmake/Koral")
        list(APPEND CMAKE_PREFIX_PATH "${KORAL_FRAMEWORK_DIR}/stage/${KORAL_ENGINE_SDK_KIND}")
        message(STATUS "Koral SDK: ${KORAL_FRAMEWORK_DIR}/stage/${KORAL_ENGINE_SDK_KIND}")
    else()
        message(WARNING "The Koral checkout at ${KORAL_FRAMEWORK_DIR} has no ${KORAL_ENGINE_SDK_KIND} SDK staged: build its `stage` target in that configuration.")
    endif()
endif()
find_package(Koral CONFIG QUIET)
if(Koral_FOUND)
    message(STATUS "Koral ${Koral_VERSION} found: building what needs it")
    add_subdirectory(modules/kgraph)
    add_subdirectory(modules/keditor)
    add_subdirectory(modules/kengine)

    # Scripts in C#: the engine's C# layer and the program that runs a project with it. Built where there is a .NET
    # SDK and the Koral SDK has its C# assemblies.
    find_program(KORAL_ENGINE_DOTNET dotnet)
    get_filename_component(KORAL_ENGINE_SDK "${Koral_DIR}/../../.." ABSOLUTE)
    if(KORAL_ENGINE_DOTNET AND EXISTS "${KORAL_ENGINE_SDK}/lib/koral-dotnet/Koral.dll")
        set(KORAL_ENGINE_DOTNET_OUT "${CMAKE_BINARY_DIR}/dotnet")
        file(GLOB_RECURSE KORAL_ENGINE_CS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/bindings/csharp/*.cs" "${CMAKE_SOURCE_DIR}/bindings/csharp/*.csproj")
        list(FILTER KORAL_ENGINE_CS EXCLUDE REGEX "/(bin|obj)/")
        add_custom_command(OUTPUT "${KORAL_ENGINE_DOTNET_OUT}/koral-engine-dotnet.dll" "${KORAL_ENGINE_DOTNET_OUT}/Koral.Engine.Tests.dll"
            COMMAND ${KORAL_ENGINE_DOTNET} build "${CMAKE_SOURCE_DIR}/bindings/csharp/Koral.Engine.Runner/Koral.Engine.Runner.csproj"
                    "-p:KoralSdk=${KORAL_ENGINE_SDK}" -o "${KORAL_ENGINE_DOTNET_OUT}" --nologo -v quiet
            COMMAND ${KORAL_ENGINE_DOTNET} build "${CMAKE_SOURCE_DIR}/bindings/csharp/Koral.Engine.Tests/Koral.Engine.Tests.csproj"
                    "-p:KoralSdk=${KORAL_ENGINE_SDK}" -o "${KORAL_ENGINE_DOTNET_OUT}" --nologo -v quiet
            DEPENDS ${KORAL_ENGINE_CS}
            COMMENT "Building the engine's C# layer"
            VERBATIM)
        add_custom_target(koral-engine-dotnet ALL DEPENDS "${KORAL_ENGINE_DOTNET_OUT}/koral-engine-dotnet.dll" koral-engine)
        # Beside the assemblies, where they look for it.
        add_custom_command(TARGET koral-engine-dotnet POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:koral-engine>" "${KORAL_ENGINE_DOTNET_OUT}/")
        if(KORAL_ENGINE_BUILD_TESTS)
            add_test(NAME CSharp.Scripts COMMAND ${KORAL_ENGINE_DOTNET} "${KORAL_ENGINE_DOTNET_OUT}/Koral.Engine.Tests.dll")
            set_tests_properties(CSharp.Scripts PROPERTIES ENVIRONMENT "KORAL_SDK=${KORAL_ENGINE_SDK}")
            # E6's gate in C#, on the project the engine's own gate saves.
            add_test(NAME CSharp.ShadowGate COMMAND ${KORAL_ENGINE_DOTNET} "${KORAL_ENGINE_DOTNET_OUT}/Koral.Engine.Tests.dll" shadow "${CMAKE_BINARY_DIR}/shadow-project")
            set_tests_properties(CSharp.ShadowGate PROPERTIES ENVIRONMENT "KORAL_SDK=${KORAL_ENGINE_SDK}" FIXTURES_REQUIRED shadow_project)
        endif()
        message(STATUS "C# scripts: building koral-engine-dotnet")
    else()
        message(STATUS "C# scripts: not built (no dotnet, or a Koral SDK without its C# assemblies)")
    endif()

    # Scripts in Kotlin: the engine's Kotlin library, published as a Maven artifact for a project's scripts to depend
    # on. Built where there is a JDK 25 (KORAL_ENGINE_JAVA_HOME, JAVA_HOME, or ~/.local/jdk) and the Koral SDK has
    # its Kotlin libraries.
    set(KORAL_ENGINE_JAVA_HOME "" CACHE PATH "The JDK (25 or later) the engine's Kotlin layer is built with")
    if(NOT KORAL_ENGINE_JAVA_HOME)
        file(GLOB KORAL_ENGINE_JDKS "$ENV{HOME}/.local/jdk/jdk-2[5-9]*")
        foreach(home IN ITEMS "$ENV{JAVA_HOME}" LISTS KORAL_ENGINE_JDKS)
            if(home AND EXISTS "${home}/bin/java")
                set(KORAL_ENGINE_JAVA_HOME "${home}" CACHE PATH "The JDK (25 or later) the engine's Kotlin layer is built with" FORCE)
                break()
            endif()
        endforeach()
    endif()
    if(KORAL_ENGINE_JAVA_HOME AND EXISTS "${KORAL_ENGINE_SDK}/share/Koral/maven/koral/koral")
        set(KORAL_ENGINE_KOTLIN_DIR "${CMAKE_SOURCE_DIR}/bindings/kotlin")
        set(KORAL_ENGINE_MAVEN "${CMAKE_BINARY_DIR}/maven")
        set(KORAL_ENGINE_GRADLE ${CMAKE_COMMAND} -E env "JAVA_HOME=${KORAL_ENGINE_JAVA_HOME}" "${KORAL_ENGINE_KOTLIN_DIR}/gradlew"
            -p "${KORAL_ENGINE_KOTLIN_DIR}" --console=plain -q "-PkoralSdk=${KORAL_ENGINE_SDK}" "-PengineRepo=${KORAL_ENGINE_MAVEN}"
            "-PengineLibrary=$<TARGET_FILE:koral-engine>")
        file(GLOB_RECURSE KORAL_ENGINE_KT CONFIGURE_DEPENDS "${KORAL_ENGINE_KOTLIN_DIR}/koral-engine/*.kt" "${KORAL_ENGINE_KOTLIN_DIR}/*.kts")
        add_custom_command(OUTPUT "${KORAL_ENGINE_MAVEN}/koral/koral-engine/maven-metadata.xml"
            COMMAND ${KORAL_ENGINE_GRADLE} :koral-engine:publishAllPublicationsToEngineRepository
            DEPENDS ${KORAL_ENGINE_KT}
            COMMENT "Building the engine's Kotlin layer"
            VERBATIM)
        add_custom_target(koral-engine-kotlin ALL DEPENDS "${KORAL_ENGINE_MAVEN}/koral/koral-engine/maven-metadata.xml")
        if(KORAL_ENGINE_BUILD_TESTS)
            # Its scripts test, and E6's gate in Kotlin on the project the engine's own gate saves.
            add_test(NAME Kotlin.Scripts COMMAND ${KORAL_ENGINE_GRADLE} :tests:test "-PshadowProject=${CMAKE_BINARY_DIR}/shadow-project")
            set_tests_properties(Kotlin.Scripts PROPERTIES FIXTURES_REQUIRED shadow_project)
        endif()
        message(STATUS "Kotlin scripts: building koral-engine's Kotlin library with ${KORAL_ENGINE_JAVA_HOME}")
    else()
        message(STATUS "Kotlin scripts: not built (no JDK 25, or a Koral SDK without its Kotlin libraries)")
    endif()

    if(KORAL_ENGINE_BUILD_TESTS)
        # The gate's project, made by the engine's own gate: what the C# and Kotlin gates open.
        add_test(NAME Shadow.Project COMMAND kengine_tests "--gtest_filter=ShadowGate.*")
        set_tests_properties(Shadow.Project PROPERTIES FIXTURES_SETUP shadow_project)
    endif()

    add_executable(koral-editor apps/editor/main.cpp)
    target_link_libraries(koral-editor PRIVATE keditor)
    add_executable(koral-play apps/player/main.cpp)
    target_link_libraries(koral-play PRIVATE keditor)
else()
    message(STATUS "Koral not found (set CMAKE_PREFIX_PATH to an SDK): building kecs alone")
endif()

# Where this build's pieces are, for the editor to write into a project's Kotlin build: the SDK, the engine's library
# and its Kotlin artifact, the JDK, and the Gradle wrapper it copies. A build's own paths: an installed editor would
# be told another way.
if(TARGET keditor)
    get_filename_component(KEDITOR_SDK_PATH "${Koral_DIR}/../../.." ABSOLUTE)
    target_compile_definitions(keditor PRIVATE
        KEDITOR_SDK="${KEDITOR_SDK_PATH}"
        KEDITOR_MAVEN="${CMAKE_BINARY_DIR}/maven"
        KEDITOR_ENGINE_LIBRARY="$<TARGET_FILE:koral-engine>"
        KEDITOR_JAVA_HOME="${KORAL_ENGINE_JAVA_HOME}"
        KEDITOR_GRADLE_WRAPPER="${CMAKE_SOURCE_DIR}/bindings/kotlin"
        KEDITOR_DOTNET_RUNNER="${CMAKE_BINARY_DIR}/dotnet/koral-engine-dotnet")
endif()
