cmake_minimum_required(VERSION 3.28)
project(KoralEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(KORAL_ENGINE_BUILD_TESTS "Build the engine's tests" ON)
# The Forward+ sample's tests take minutes each: built always, as keditor_forward_plus_tests, but part of a ctest run
# only when asked for — -DKORAL_ENGINE_FORWARD_PLUS_TESTS=ON — or run by hand.
option(KORAL_ENGINE_FORWARD_PLUS_TESTS "Run the Forward+ sample's slow tests with ctest" OFF)
# Where the editor offers to make a new project: the KORAL_ENGINE_PROJECTS environment variable says otherwise.
if(WIN32)
    file(TO_CMAKE_PATH "$ENV{USERPROFILE}/KoralProjects" KORAL_ENGINE_PROJECTS_DEFAULT)
else()
    set(KORAL_ENGINE_PROJECTS_DEFAULT "/mnt/w/EngineProjects")
endif()
set(KORAL_ENGINE_PROJECTS "${KORAL_ENGINE_PROJECTS_DEFAULT}" CACHE PATH "The folder new projects are made in, unless another is picked")
# Address and undefined-behaviour sanitizers, for the tests: -DKORAL_ENGINE_SANITIZE=ON.
option(KORAL_ENGINE_SANITIZE "Build with ASan and UBSan" OFF)
if(KORAL_ENGINE_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
elseif(KORAL_ENGINE_SANITIZE)
    # MSVC has ASan only (no UBSan), and not with incremental linking or edit-and-continue.
    add_compile_options(/fsanitize=address)
    string(REPLACE "/ZI" "/Zi" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    add_link_options(/INCREMENTAL:NO)
endif()

if(KORAL_ENGINE_BUILD_TESTS)
    enable_testing()
    # Eight test programs at a time, each its own process: the whole run in under five minutes. Those that time
    # themselves are RUN_SERIAL, and each program has a temporary folder of its own (kgraph/tests/support.cpp).
    set(CMAKE_CTEST_ARGUMENTS "--parallel;8;--output-on-failure")
    find_package(GTest REQUIRED)
    include(GoogleTest)
endif()

add_subdirectory(modules/kecs)
add_subdirectory(modules/kasset)
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
# The cache's CMAKE_PREFIX_PATH is the one given: a toolchain such as vcpkg's fills the variable by itself.
if(KORAL_FRAMEWORK_DIR AND NOT Koral_DIR AND NOT "$CACHE{CMAKE_PREFIX_PATH}")
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
    get_filename_component(KORAL_ENGINE_SDK "${Koral_DIR}/../../.." ABSOLUTE)

    # Windows finds a program's DLLs beside it, not by an RPATH: a program that runs Koral gets the SDK's bin/ beside
    # it — modules/ kept apart, where Koral looks for them by name — and the DLLs it links, the modules among them.
    # Koral finds its shaders and assets from where Koral.dll is, so they come too: beside it is a layout it knows.
    # Called where the program is made, as a target's build steps must be.
    function(koral_engine_runtime program)
        if(NOT WIN32)
            return()
        endif()
        koral_copy_runtime_dlls(${program})
        file(GLOB runtime "${KORAL_ENGINE_SDK}/bin/*.bin" "${KORAL_ENGINE_SDK}/bin/*.manifest")
        # Linked again when the SDK is staged again, so that what is copied after the link is the SDK's as it is now, not
        # what was beside the program before.
        file(GLOB sdkLibraries CONFIGURE_DEPENDS "${KORAL_ENGINE_SDK}/bin/*.dll" "${KORAL_ENGINE_SDK}/bin/modules/*.dll")
        set_property(TARGET ${program} APPEND PROPERTY LINK_DEPENDS ${sdkLibraries})
        # Programs made in one folder copy into the same one after they link: one at a time, or a copy finds a file
        # another is writing (Ninja's pools; other generators link a folder's programs one after another anyway).
        string(MD5 folder "${CMAKE_CURRENT_BINARY_DIR}")
        string(SUBSTRING "${folder}" 0 12 folder)
        get_property(pools GLOBAL PROPERTY JOB_POOLS)
        if(NOT "runtime_${folder}=1" IN_LIST pools)
            set_property(GLOBAL APPEND PROPERTY JOB_POOLS "runtime_${folder}=1")
        endif()
        set_property(TARGET ${program} PROPERTY JOB_POOL_LINK "runtime_${folder}")
        add_custom_command(TARGET ${program} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${runtime} $<TARGET_RUNTIME_DLLS:${program}> "$<TARGET_FILE_DIR:${program}>"
            COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${KORAL_ENGINE_SDK}/bin/modules" "$<TARGET_FILE_DIR:${program}>/modules"
            COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${KORAL_ENGINE_SDK}/share/Koral/shaders" "$<TARGET_FILE_DIR:${program}>/shaders"
            COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${KORAL_ENGINE_SDK}/share/Koral/assets" "$<TARGET_FILE_DIR:${program}>/assets"
            COMMAND_EXPAND_LISTS
            VERBATIM)
    endfunction()
    add_subdirectory(modules/kasset-import)
    add_subdirectory(modules/kgraph)
    add_subdirectory(modules/keditor)
    add_subdirectory(modules/kengine)

    # Scripts in C#: the engine's C# layer, and what koral-editor and koral-play start .NET with to run a project's
    # scripts inside them (Koral.Engine.Host), in dotnet/ beside them. Built where there is a .NET SDK and the Koral
    # SDK has its C# assemblies.
    find_program(KORAL_ENGINE_DOTNET dotnet)
    if(KORAL_ENGINE_DOTNET AND EXISTS "${KORAL_ENGINE_SDK}/lib/koral-dotnet/Koral.dll")
        set(KORAL_ENGINE_DOTNET_OUT "${CMAKE_BINARY_DIR}/dotnet")
        file(GLOB_RECURSE KORAL_ENGINE_CS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/bindings/csharp/*.cs" "${CMAKE_SOURCE_DIR}/bindings/csharp/*.csproj")
        list(FILTER KORAL_ENGINE_CS EXCLUDE REGEX "/(bin|obj)/")
        add_custom_command(OUTPUT "${KORAL_ENGINE_DOTNET_OUT}/Koral.Engine.Host.dll" "${KORAL_ENGINE_DOTNET_OUT}/Koral.Engine.Tests.dll"
            COMMAND ${KORAL_ENGINE_DOTNET} build "${CMAKE_SOURCE_DIR}/bindings/csharp/Koral.Engine.Host/Koral.Engine.Host.csproj"
                    "-p:KoralSdk=${KORAL_ENGINE_SDK}" -o "${KORAL_ENGINE_DOTNET_OUT}" --nologo -v quiet
            COMMAND ${KORAL_ENGINE_DOTNET} build "${CMAKE_SOURCE_DIR}/bindings/csharp/Koral.Engine.Tests/Koral.Engine.Tests.csproj"
                    "-p:KoralSdk=${KORAL_ENGINE_SDK}" -o "${KORAL_ENGINE_DOTNET_OUT}" --nologo -v quiet
            # Up to date for what is below, though nothing of the engine's needed compiling again.
            COMMAND ${CMAKE_COMMAND} -E touch "${KORAL_ENGINE_DOTNET_OUT}/Koral.Engine.Host.dll" "${KORAL_ENGINE_DOTNET_OUT}/Koral.Engine.Tests.dll"
            # The SDK's assemblies too: they are copied beside the host, and a script is compiled against them.
            DEPENDS ${KORAL_ENGINE_CS} "${KORAL_ENGINE_SDK}/lib/koral-dotnet/Koral.dll"
                    "${KORAL_ENGINE_SDK}/lib/koral-dotnet/Koral.UI.dll" "${KORAL_ENGINE_SDK}/lib/koral-dotnet/Koral.Net.dll"
            COMMENT "Building the engine's C# layer"
            VERBATIM)
        add_custom_target(koral-engine-dotnet ALL DEPENDS "${KORAL_ENGINE_DOTNET_OUT}/Koral.Engine.Host.dll" koral-engine)
        # Beside the assemblies, where they look for it.
        add_custom_command(TARGET koral-engine-dotnet POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:koral-engine>" "${KORAL_ENGINE_DOTNET_OUT}/")
        if(KORAL_ENGINE_BUILD_TESTS)
            add_test(NAME CSharp.Scripts COMMAND ${KORAL_ENGINE_DOTNET} "${KORAL_ENGINE_DOTNET_OUT}/Koral.Engine.Tests.dll")
            set_tests_properties(CSharp.Scripts PROPERTIES ENVIRONMENT "KORAL_SDK=${KORAL_ENGINE_SDK};DOTNET_MODIFIABLE_ASSEMBLIES=debug")
            # E6's gate in C#, on the project the engine's own gate saves.
            add_test(NAME CSharp.ShadowGate COMMAND ${KORAL_ENGINE_DOTNET} "${KORAL_ENGINE_DOTNET_OUT}/Koral.Engine.Tests.dll" shadow "${CMAKE_BINARY_DIR}/shadow-project")
            set_tests_properties(CSharp.ShadowGate PROPERTIES ENVIRONMENT "KORAL_SDK=${KORAL_ENGINE_SDK}" FIXTURES_REQUIRED shadow_project
                RESOURCE_LOCK shadow_project)     # it writes the project's scripts: the Kotlin gate opens the project too
        endif()
        message(STATUS "C# scripts: building the engine's C# layer, which koral-editor and koral-play run them with")
    else()
        message(STATUS "C# scripts: not built (no dotnet, or a Koral SDK without its C# assemblies)")
    endif()

    # Scripts in Kotlin: the engine's Kotlin library, published as a Maven artifact for a project's scripts to depend
    # on. Built where there is a JDK 25 (KORAL_ENGINE_JAVA_HOME, JAVA_HOME, or ~/.local/jdk) and the Koral SDK has
    # its Kotlin libraries.
    set(KORAL_ENGINE_JAVA_HOME "" CACHE PATH "The JDK (25 or later) the engine's Kotlin layer is built with")
    if(NOT KORAL_ENGINE_JAVA_HOME)
        file(GLOB KORAL_ENGINE_JDKS "$ENV{HOME}/.local/jdk/jdk-2[5-9]*")
        file(TO_CMAKE_PATH "$ENV{JAVA_HOME}" KORAL_ENGINE_JAVA_HOME_ENV)     # it goes into a C++ string too: no backslashes
        foreach(home IN ITEMS "${KORAL_ENGINE_JAVA_HOME_ENV}" LISTS KORAL_ENGINE_JDKS)
            if(home AND EXISTS "${home}/bin/java${CMAKE_EXECUTABLE_SUFFIX}")
                set(KORAL_ENGINE_JAVA_HOME "${home}" CACHE PATH "The JDK (25 or later) the engine's Kotlin layer is built with" FORCE)
                break()
            endif()
        endforeach()
    endif()
    if(KORAL_ENGINE_JAVA_HOME AND EXISTS "${KORAL_ENGINE_SDK}/share/Koral/maven/koral/koral")
        set(KORAL_ENGINE_KOTLIN_DIR "${CMAKE_SOURCE_DIR}/bindings/kotlin")
        set(KORAL_ENGINE_MAVEN "${CMAKE_BINARY_DIR}/maven")
        set(KORAL_ENGINE_GRADLEW "${KORAL_ENGINE_KOTLIN_DIR}/gradlew")
        if(WIN32)
            string(APPEND KORAL_ENGINE_GRADLEW ".bat")
        endif()
        # No daemon of Gradle's or Kotlin's: one left running keeps this step's output open, and Ninja waits on it for good.
        set(KORAL_ENGINE_GRADLE ${CMAKE_COMMAND} -E env "JAVA_HOME=${KORAL_ENGINE_JAVA_HOME}" "${KORAL_ENGINE_GRADLEW}"
            -p "${KORAL_ENGINE_KOTLIN_DIR}" --no-daemon -Pkotlin.compiler.execution.strategy=in-process
            --console=plain -q "-PkoralSdk=${KORAL_ENGINE_SDK}" "-PengineRepo=${KORAL_ENGINE_MAVEN}"
            "-PengineLibrary=$<TARGET_FILE:koral-engine>")
        file(GLOB_RECURSE KORAL_ENGINE_KT CONFIGURE_DEPENDS "${KORAL_ENGINE_KOTLIN_DIR}/koral-engine/*.kt" "${KORAL_ENGINE_KOTLIN_DIR}/*.kts")
        add_custom_command(OUTPUT "${KORAL_ENGINE_MAVEN}/koral/koral-engine/maven-metadata.xml"
            COMMAND ${KORAL_ENGINE_GRADLE} :koral-engine:publishAllPublicationsToEngineRepository
            DEPENDS ${KORAL_ENGINE_KT}
            COMMENT "Building the engine's Kotlin layer"
            VERBATIM)
        # What koral-editor and koral-play start a JVM with to run a project's scripts inside them, in kotlin/ beside
        # them: the engine's Kotlin layer and what it runs on (lib), and Kotlin's compiler (compiler).
        set(KORAL_ENGINE_KOTLIN_OUT "${CMAKE_BINARY_DIR}/kotlin")
        add_custom_command(OUTPUT "${KORAL_ENGINE_KOTLIN_OUT}/lib/koral-engine.jar"
            COMMAND ${KORAL_ENGINE_GRADLE} :koral-engine:hostLayout "-PhostDir=${KORAL_ENGINE_KOTLIN_OUT}"
            DEPENDS ${KORAL_ENGINE_KT} "${KORAL_ENGINE_MAVEN}/koral/koral-engine/maven-metadata.xml"
            COMMENT "Laying out the engine's Kotlin layer, and Kotlin's compiler, for the engine's programs"
            VERBATIM)
        add_custom_target(koral-engine-kotlin ALL DEPENDS "${KORAL_ENGINE_MAVEN}/koral/koral-engine/maven-metadata.xml" "${KORAL_ENGINE_KOTLIN_OUT}/lib/koral-engine.jar")
        if(KORAL_ENGINE_BUILD_TESTS)
            # Its scripts test, and E6's gate in Kotlin on the project the engine's own gate saves.
            add_test(NAME Kotlin.Scripts COMMAND ${KORAL_ENGINE_GRADLE} :tests:test "-PshadowProject=${CMAKE_BINARY_DIR}/shadow-project")
            set_tests_properties(Kotlin.Scripts PROPERTIES FIXTURES_REQUIRED shadow_project RESOURCE_LOCK shadow_project)
        endif()
        message(STATUS "Kotlin scripts: building koral-engine's Kotlin library with ${KORAL_ENGINE_JAVA_HOME}")
    else()
        message(STATUS "Kotlin scripts: not built (no JDK 25, or a Koral SDK without its Kotlin libraries)")
    endif()
    # The runtimes a project's scripts run on, beside the programs, for a machine that has neither: .NET's — the newest
    # 10 there is where `dotnet` is — in dotnet/ (host/fxr and shared/Microsoft.NETCore.App, beside the engine's own
    # assemblies), and a JRE of the JDK's in jre/ (jlink: what the engine's Kotlin layer and Kotlin's compiler need).
    # khost looks in both first. About 150 MB, so asked for: -DKORAL_ENGINE_SHIP_RUNTIMES=ON.
    option(KORAL_ENGINE_SHIP_RUNTIMES "Put a .NET runtime in dotnet/ and a JRE in jre/ beside the programs" OFF)
    if(KORAL_ENGINE_SHIP_RUNTIMES)
        if(KORAL_ENGINE_DOTNET)
            get_filename_component(KORAL_ENGINE_DOTNET_ROOT "${KORAL_ENGINE_DOTNET}" DIRECTORY)
            file(GLOB fxrs LIST_DIRECTORIES true "${KORAL_ENGINE_DOTNET_ROOT}/host/fxr/10.*")
            file(GLOB runtimes LIST_DIRECTORIES true "${KORAL_ENGINE_DOTNET_ROOT}/shared/Microsoft.NETCore.App/10.*")
            list(SORT fxrs COMPARE NATURAL ORDER DESCENDING)
            list(SORT runtimes COMPARE NATURAL ORDER DESCENDING)
            if(fxrs AND runtimes)
                list(GET fxrs 0 fxr)
                list(GET runtimes 0 runtime)
                get_filename_component(fxrVersion "${fxr}" NAME)
                get_filename_component(runtimeVersion "${runtime}" NAME)
                add_custom_target(koral-engine-dotnet-runtime ALL
                    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${fxr}" "${CMAKE_BINARY_DIR}/dotnet/host/fxr/${fxrVersion}"
                    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${runtime}" "${CMAKE_BINARY_DIR}/dotnet/shared/Microsoft.NETCore.App/${runtimeVersion}"
                    COMMENT "Shipping .NET ${runtimeVersion} in dotnet/"
                    VERBATIM)
                message(STATUS "Runtimes: .NET ${runtimeVersion} shipped in dotnet/")
            else()
                message(WARNING "KORAL_ENGINE_SHIP_RUNTIMES: no .NET 10 runtime beside ${KORAL_ENGINE_DOTNET}")
            endif()
        endif()
        if(KORAL_ENGINE_JAVA_HOME AND EXISTS "${KORAL_ENGINE_JAVA_HOME}/bin/jlink${CMAKE_EXECUTABLE_SUFFIX}")
            set(jreModules java.base,java.desktop,java.instrument,java.logging,java.management,java.rmi,java.scripting,jdk.compiler,jdk.unsupported,jdk.zipfs)
            add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/jre/release"
                COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_BINARY_DIR}/jre"
                COMMAND "${KORAL_ENGINE_JAVA_HOME}/bin/jlink" --add-modules ${jreModules} --strip-debug --no-header-files --no-man-pages
                        --output "${CMAKE_BINARY_DIR}/jre"
                COMMENT "Shipping a JRE in jre/ (jlink, of ${KORAL_ENGINE_JAVA_HOME})"
                VERBATIM)
            add_custom_target(koral-engine-jre ALL DEPENDS "${CMAKE_BINARY_DIR}/jre/release")
            message(STATUS "Runtimes: a JRE of ${KORAL_ENGINE_JAVA_HOME} shipped in jre/")
        endif()
    endif()

    add_subdirectory(modules/khost)     # after the JDK is known: its JNI headers are what Kotlin is run with

    if(KORAL_ENGINE_BUILD_TESTS)
        # The gate's project, made by the engine's own gate: what the C# and Kotlin gates open.
        add_test(NAME Shadow.Project COMMAND kengine_tests "--gtest_filter=ShadowGate.*")
        set_tests_properties(Shadow.Project PROPERTIES FIXTURES_SETUP shadow_project RESOURCE_LOCK shadow_project)
    endif()

    add_executable(koral-editor apps/editor/main.cpp apps/frames.h)
    add_executable(koral-play apps/player/main.cpp apps/frames.h)
    foreach(app koral-editor koral-play)
        # A project's scripts run inside them, and call the engine's C interface: theirs, which they export.
        target_link_libraries(${app} PRIVATE keditor khost kengine-capi)
        target_include_directories(${app} PRIVATE apps)
        set_target_properties(${app} PROPERTIES ENABLE_EXPORTS ON)
        # What a project's scripts are run with, beside them: built with them.
        foreach(layer koral-engine-dotnet koral-engine-kotlin)
            if(TARGET ${layer})
                add_dependencies(${app} ${layer})
            endif()
        endforeach()
    endforeach()
    # The project either opens when it is given none: Forward+, made with nodes, in the projects folder (forward-plus).
    foreach(app koral-editor koral-play)
        target_compile_definitions(${app} PRIVATE KORAL_ENGINE_DEFAULT_PROJECT="${KORAL_ENGINE_PROJECTS}/forward-plus")
    endforeach()
    foreach(app koral-editor koral-play)
        koral_engine_runtime(${app})
    endforeach()
    # A release: the programs and everything they run with, in one folder for another Windows PC.
    include(cmake/Release.cmake)

    # khost, as the apps are linked with it: what finds and starts .NET and a JVM, each test a process of its own.
    if(KORAL_ENGINE_BUILD_TESTS)
        add_executable(khost_tests modules/khost/tests/test_host.cpp)
        target_link_libraries(khost_tests PRIVATE khost kengine-capi GTest::gtest GTest::gtest_main)
        set_target_properties(khost_tests PROPERTIES ENABLE_EXPORTS ON)
        if(TARGET koral-engine-dotnet)
            add_dependencies(khost_tests koral-engine-dotnet)
            target_compile_definitions(khost_tests PRIVATE KHOST_TEST_DOTNET="${CMAKE_BINARY_DIR}/dotnet")
        endif()
        if(TARGET koral-engine-kotlin)
            add_dependencies(khost_tests koral-engine-kotlin)
            target_compile_definitions(khost_tests PRIVATE KHOST_TEST_KOTLIN="${CMAKE_BINARY_DIR}/kotlin")
        endif()
        if(KORAL_ENGINE_SHIP_RUNTIMES)
            target_compile_definitions(khost_tests PRIVATE KHOST_TEST_SHIPPED)
            foreach(shipped koral-engine-dotnet-runtime koral-engine-jre)
                if(TARGET ${shipped})
                    add_dependencies(khost_tests ${shipped})
                endif()
            endforeach()
        endif()
        koral_engine_runtime(khost_tests)
        gtest_discover_tests(khost_tests DISCOVERY_MODE PRE_TEST)
    endif()
else()
    message(STATUS "Koral not found (set CMAKE_PREFIX_PATH to an SDK): building kecs alone")
endif()

# Where this build's C# assemblies are, for the editor to name in the project of a project's C# scripts that an editor
# of code reads. A build's own paths: an installed editor would be told another way.
if(TARGET keditor)
    target_compile_definitions(keditor PRIVATE
        KEDITOR_DOTNET="${CMAKE_BINARY_DIR}/dotnet"
        # And its Kotlin libraries, the Gradle wrapper and the JDK, for the Gradle description of a project's Kotlin scripts.
        KEDITOR_KOTLIN="${CMAKE_BINARY_DIR}/kotlin"
        KEDITOR_GRADLE_WRAPPER="${CMAKE_SOURCE_DIR}/bindings/kotlin"
        KEDITOR_JAVA_HOME="${KORAL_ENGINE_JAVA_HOME}"
        KEDITOR_PROJECTS="${KORAL_ENGINE_PROJECTS}")
endif()
