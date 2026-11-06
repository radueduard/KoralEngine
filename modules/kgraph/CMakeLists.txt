# kgraph: a scene's pass graph, as data and running. Needs the framework.
add_library(kgraph STATIC src/graph.cpp src/catalog.cpp src/runtime.cpp src/entities.cpp src/materials.cpp src/meshes.cpp src/scene.cpp src/blocks.cpp src/expand.cpp src/random.cpp)
add_library(KoralEngine::kgraph ALIAS kgraph)
target_include_directories(kgraph PUBLIC include)
target_link_libraries(kgraph PUBLIC kecs kasset Koral::Koral PRIVATE kasset-import Koral::koral-image-import Koral::koral-image-compress)
target_compile_options(kgraph PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>)
target_compile_options(kgraph PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)   # runtime.cpp has more sections than a COFF object holds by default

if(KORAL_ENGINE_BUILD_TESTS)
    add_executable(kgraph_tests tests/test_graph.cpp tests/test_runtime.cpp tests/test_entities.cpp tests/test_materials.cpp tests/test_draw.cpp tests/test_buffers.cpp tests/test_builder.cpp tests/test_flow.cpp tests/test_scale.cpp tests/test_random.cpp tests/support.cpp)
    target_link_libraries(kgraph_tests PRIVATE kgraph kscript GTest::gtest GTest::gtest_main)
    koral_engine_runtime(kgraph_tests)
    target_compile_definitions(kgraph_tests PRIVATE KGRAPH_TEST_SHADERS="${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders"
                                                   KGRAPH_PACKAGE_SHADERS="${PROJECT_SOURCE_DIR}/packages/koral/shaders")
    # Freed memory is filled with a pattern, and a test that never ends is stopped: an edit that frees what a frame
    # is still drawing with reads what it freed, and shows as a crash or a wait that never ends rather than as luck.
    # Those that say how long something takes run alone, so that ctest -j does not slow them by what runs beside them.
    set(KGRAPH_TIMED_TESTS "ScaleTest.*:EntityTest.ManyEntitiesEachWithAnImageOfItsOwn")
    gtest_discover_tests(kgraph_tests DISCOVERY_MODE PRE_TEST TEST_FILTER "-${KGRAPH_TIMED_TESTS}"
                         PROPERTIES ENVIRONMENT "MALLOC_PERTURB_=165" TIMEOUT 180)
    gtest_discover_tests(kgraph_tests DISCOVERY_MODE PRE_TEST TEST_FILTER "${KGRAPH_TIMED_TESTS}"
                         PROPERTIES ENVIRONMENT "MALLOC_PERTURB_=165" TIMEOUT 180 RUN_SERIAL TRUE LABELS timed)
endif()
