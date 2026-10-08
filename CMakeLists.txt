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

# What runs on the GPU needs the Koral framework: point CMAKE_PREFIX_PATH at an SDK (a Koral checkout's
# stage/debug, say). Without one, only what stands alone is built.
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
        endif()
        message(STATUS "C# scripts: building koral-engine-dotnet")
    else()
        message(STATUS "C# scripts: not built (no dotnet, or a Koral SDK without its C# assemblies)")
    endif()

    add_executable(koral-editor apps/editor/main.cpp)
    target_link_libraries(koral-editor PRIVATE keditor)
    add_executable(koral-play apps/player/main.cpp)
    target_link_libraries(koral-play PRIVATE keditor)
else()
    message(STATUS "Koral not found (set CMAKE_PREFIX_PATH to an SDK): building kecs alone")
endif()
