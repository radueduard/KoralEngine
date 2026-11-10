# A release of the engine for Windows: koral-editor and koral-play in a folder of their own with everything they run
# with beside them, for a PC that has nothing of this machine's — no Koral SDK, no source, no .NET, no JDK, no Visual
# C++ runtime. Only a GPU driver with Vulkan is asked of it.
#
#   cmake --install <release build> --prefix <folder>          the folder
#   cmake --build <release build> --target koral-engine-release  the folder, and a .zip of it, in <build>/release
#
# What a project's scripts run on comes too when the build ships the runtimes (-DKORAL_ENGINE_SHIP_RUNTIMES=ON): a .NET
# in dotnet/, a JRE in jre/. Without them, scripts run only where the PC has .NET 10 and a JDK 25 of its own.
#
# The programs find what is beside them first (kscript::ProgramDirectory): the engine's package in packages/koral, the
# modules a project imports in packages/modules, the C# and Kotlin layers in dotnet/ and kotlin/, the Gradle wrapper
# a new project's Kotlin build is given in gradle-wrapper/.
if(NOT WIN32)
    return()
endif()

install(TARGETS koral-editor koral-play RUNTIME DESTINATION .)

# The DLLs the programs load, as the build put them beside them — the SDK's, vcpkg's — but not the tests' or Vulkan's
# loader: the GPU driver installs that one, newer than any copied here.
set(KORAL_RELEASE_BINARY_DIR "${CMAKE_BINARY_DIR}")
set(KORAL_RELEASE_SDK "${KORAL_ENGINE_SDK}")
string(CONFIGURE [[
    file(GLOB dlls "@KORAL_RELEASE_BINARY_DIR@/*.dll")
    list(FILTER dlls EXCLUDE REGEX "/(gtest|gtest_main|gmock|gmock_main|vulkan-1)\\.dll$")
    file(GLOB extra "@KORAL_RELEASE_SDK@/bin/*.bin" "@KORAL_RELEASE_SDK@/bin/*.manifest")
    file(INSTALL ${dlls} ${extra} DESTINATION "${CMAKE_INSTALL_PREFIX}")
]] code @ONLY)
install(CODE "${code}")

# The Koral SDK's modules, shaders and assets: Koral finds them where Koral.dll is.
install(DIRECTORY "${KORAL_ENGINE_SDK}/bin/modules/" DESTINATION modules FILES_MATCHING PATTERN "*.dll")
install(DIRECTORY "${KORAL_ENGINE_SDK}/share/Koral/shaders/" DESTINATION shaders)
install(DIRECTORY "${KORAL_ENGINE_SDK}/share/Koral/assets/" DESTINATION assets)

# The engine's package, and the modules a project can import.
install(DIRECTORY "${CMAKE_SOURCE_DIR}/packages/" DESTINATION packages
        PATTERN "bin" EXCLUDE PATTERN "obj" EXCLUDE PATTERN ".koral" EXCLUDE)

# What a project's scripts are run with, and — shipped — run on.
foreach(layer dotnet kotlin jre)
    install(DIRECTORY "${CMAKE_BINARY_DIR}/${layer}/" DESTINATION ${layer} OPTIONAL
            PATTERN "Koral.Engine.Tests*" EXCLUDE PATTERN "*.pdb" EXCLUDE)
endforeach()
install(FILES "${CMAKE_SOURCE_DIR}/bindings/kotlin/gradlew" "${CMAKE_SOURCE_DIR}/bindings/kotlin/gradlew.bat" DESTINATION gradle-wrapper)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/bindings/kotlin/gradle/wrapper/" DESTINATION gradle-wrapper/gradle/wrapper)

# The Visual C++ runtime, beside the programs: a PC without its redistributable runs them all the same.
set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
set(CMAKE_INSTALL_UCRT_LIBRARIES OFF)       # Windows 10 and 11 have the universal CRT
include(InstallRequiredSystemLibraries)

install(FILES "${CMAKE_SOURCE_DIR}/cmake/README-release.txt" DESTINATION . RENAME README.txt)

# The folder and its .zip, in one step.
set(KORAL_RELEASE_DIR "${CMAKE_BINARY_DIR}/release/KoralEngine")
add_custom_target(koral-engine-release
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${KORAL_RELEASE_DIR}"
    COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}" --prefix "${KORAL_RELEASE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_BINARY_DIR}/release" ${CMAKE_COMMAND} -E tar cf KoralEngine-windows-x64.zip --format=zip KoralEngine
    DEPENDS koral-editor koral-play
    COMMENT "A release of the engine in ${CMAKE_BINARY_DIR}/release: KoralEngine/, and KoralEngine-windows-x64.zip"
    VERBATIM)
