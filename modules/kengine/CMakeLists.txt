# The engine's C interface, compiled once: in the engine's library, and in the engine's own programs, which export it
# for the scripts they run inside them (khost) — the program's own copy of the engine, not a library's beside it.
add_library(kengine-capi OBJECT src/capi.cpp)
target_include_directories(kengine-capi PUBLIC include)
target_compile_definitions(kengine-capi PUBLIC KENGINE_BUILD)
target_link_libraries(kengine-capi PUBLIC keditor kgraph kscript kecs Koral::Koral)
target_compile_options(kengine-capi PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>)

# koral-engine: the engine as one shared library, with its C interface and its scenes. What a C# or Kotlin program
# loads beside the framework.
add_library(koral-engine SHARED src/scenes.cpp $<TARGET_OBJECTS:kengine-capi>)
add_library(KoralEngine::koral-engine ALIAS koral-engine)
target_include_directories(koral-engine PUBLIC include)
target_compile_definitions(koral-engine PRIVATE KENGINE_BUILD)
target_link_libraries(koral-engine PRIVATE keditor kgraph kscript kecs PUBLIC Koral::Koral)
koral_engine_runtime(koral-engine)   # its modules are its own, linked privately: kengine_tests, beside it, would not have them
target_compile_options(koral-engine PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>)
if(MSVC)
    # No manifest of its own, as Koral's modules have none: loaded from C#, its import of Koral.dll is then resolved by the
    # manifest beside Koral (NativeLibraryResolver.LoadBesideKoral), not to the managed assembly of that name.
    target_link_options(koral-engine PRIVATE /MANIFEST:NO)
endif()

if(KORAL_ENGINE_BUILD_TESTS)
    # Only what a program in another language has: the framework, and this library through its C interface.
    add_executable(kengine_tests tests/test_capi.cpp tests/test_shadow.cpp)
    target_link_libraries(kengine_tests PRIVATE koral-engine Koral::Koral GTest::gtest GTest::gtest_main)
    koral_engine_runtime(kengine_tests)
    target_compile_definitions(kengine_tests PRIVATE
        KENGINE_LIBRARY="$<TARGET_FILE:koral-engine>"
        KEDITOR_TEST_SHADERS="${CMAKE_SOURCE_DIR}/modules/keditor/tests/shaders"
        KENGINE_SHADOW_SHADERS="${CMAKE_CURRENT_SOURCE_DIR}/tests/shadow/shaders"
        KENGINE_SHADOW_PROJECT="${CMAKE_BINARY_DIR}/shadow-project")
    # The gate writes the project the C# and Kotlin gates open (Shadow.Project runs it to make it): one at a time with them.
    gtest_discover_tests(kengine_tests DISCOVERY_MODE PRE_TEST PROPERTIES RESOURCE_LOCK shadow_project)
endif()
