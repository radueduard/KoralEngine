# kphysics: rigid bodies on the CPU. Stands alone — no GPU, no framework, not even kecs — so it is tested without any.
add_library(kphysics STATIC
        src/bodies.cpp
        src/contacts.cpp
        src/narrowphase.cpp
        src/convex.cpp
        src/mesh.cpp
        src/dynamic_tree.cpp
        src/broadphase.cpp
        src/solver.cpp
        src/joints.cpp
        src/islands.cpp
        src/simulation.cpp
        src/recording.cpp
        src/thread_pool.cpp)
add_library(KoralEngine::kphysics ALIAS kphysics)
target_include_directories(kphysics PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(kphysics PUBLIC Threads::Threads)
# No -ffast-math, ever: the reference runs are compared within a tolerance, and a run must repeat itself to the bit.
target_compile_options(kphysics PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>)

if(KORAL_ENGINE_BUILD_TESTS)
    add_executable(kphysics_tests
            tests/support.cpp
            tests/test_math.cpp
            tests/test_bodies.cpp
            tests/test_integrator.cpp
            tests/test_narrowphase.cpp
            tests/test_box_box.cpp
            tests/test_broadphase.cpp
            tests/test_solver.cpp
            tests/test_sleep.cpp
            tests/test_joints.cpp
            tests/test_reference.cpp
            tests/test_threads.cpp
            tests/test_cones_meshes.cpp)
    target_link_libraries(kphysics_tests PRIVATE kphysics GTest::gtest GTest::gtest_main)
    # The Kotlin core's reference runs, which this port is checked against.
    target_compile_definitions(kphysics_tests PRIVATE KPHYSICS_REFERENCE_RUNS="${CMAKE_CURRENT_SOURCE_DIR}/tests/reference")
    gtest_discover_tests(kphysics_tests DISCOVERY_MODE PRE_TEST)

    # How fast: the same scenes as the Kotlin core's MeasureTest and broadphase benchmark. Not a test; run it.
    add_executable(kphysics_bench tests/bench.cpp tests/support.cpp)
    target_link_libraries(kphysics_bench PRIVATE kphysics)
endif()
