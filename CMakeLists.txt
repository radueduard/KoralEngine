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

# What runs on the GPU needs the Koral framework: point CMAKE_PREFIX_PATH at an SDK (a Koral checkout's
# stage/debug, say). Without one, only what stands alone is built.
find_package(Koral CONFIG QUIET)
if(Koral_FOUND)
    message(STATUS "Koral ${Koral_VERSION} found: building what needs it")
    add_subdirectory(modules/kgraph)
else()
    message(STATUS "Koral not found (set CMAKE_PREFIX_PATH to an SDK): building kecs alone")
endif()
