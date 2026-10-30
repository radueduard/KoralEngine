# keditor: a scene being worked on, the project it is in, and the editor over them. Needs the framework and its
# interface modules.
# The standard shaders, as text in the library: what a new project's shaders folder is given (default), and the lit
# graph's (engineCommon, lit, sky).
set(KEDITOR_SHADER_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/standard_shaders.h)
set(KEDITOR_SHADER_TEXT "#pragma once\n\nnamespace keditor::detail\n{\n")
foreach(shader default engineCommon lit sky)
    set(path ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene/${shader}.slang)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${path})
    file(READ ${path} source)
    string(SUBSTRING ${shader} 0 1 first)
    string(TOUPPER ${first} first)
    string(SUBSTRING ${shader} 1 -1 rest)
    string(APPEND KEDITOR_SHADER_TEXT "    inline constexpr const char* ${first}${rest}Slang = R\"koralslang(${source})koralslang\";\n")
endforeach()
string(APPEND KEDITOR_SHADER_TEXT "}\n")
file(GENERATE OUTPUT ${KEDITOR_SHADER_HEADER} CONTENT "${KEDITOR_SHADER_TEXT}")

# The Assets panel's shader icons, a page and a small one for each stage: SVGs as text in the library, by file name.
set(KEDITOR_ICON_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/shader_icons.h)
set(KEDITOR_ICON_TEXT "#pragma once\n\n#include <string_view>\n#include <utility>\n\nnamespace keditor::detail\n{\n    inline constexpr std::pair<std::string_view, std::string_view> ShaderIcons[] {\n")
file(GLOB KEDITOR_ICONS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/icons/shaders/*.svg)
foreach(path ${KEDITOR_ICONS})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${path})
    file(READ ${path} source)
    get_filename_component(name ${path} NAME_WE)
    string(APPEND KEDITOR_ICON_TEXT "        { \"${name}\", R\"koralsvg(${source})koralsvg\" },\n")
endforeach()
string(APPEND KEDITOR_ICON_TEXT "    };\n}\n")
file(GENERATE OUTPUT ${KEDITOR_ICON_HEADER} CONTENT "${KEDITOR_ICON_TEXT}")

add_library(keditor STATIC src/document.cpp src/project.cpp src/play.cpp src/editor.cpp src/assets.cpp src/widgets.cpp src/starter.cpp src/look.cpp src/overlay.cpp src/viewer.cpp src/physics.cpp)
target_include_directories(keditor PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_library(KoralEngine::keditor ALIAS keditor)
target_include_directories(keditor PUBLIC include)
target_link_libraries(keditor PUBLIC kgraph kecs kasset kscript Koral::Koral Koral::koral-ui Koral::koral-gui-extras PRIVATE kasset-import Koral::koral-image-import kphysics)
target_compile_options(keditor PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wno-missing-field-initializers>)
target_compile_options(keditor PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)  # editor.cpp, as kgraph's runtime.cpp

if(KORAL_ENGINE_BUILD_TESTS)
    add_executable(keditor_tests tests/test_document.cpp tests/test_editor.cpp tests/test_forward_plus.cpp tests/test_physics.cpp ${CMAKE_SOURCE_DIR}/modules/kgraph/tests/support.cpp)
    target_include_directories(keditor_tests PRIVATE ${CMAKE_SOURCE_DIR}/modules/kgraph/tests ${CMAKE_SOURCE_DIR}/samples/forward-plus)
    target_link_libraries(keditor_tests PRIVATE keditor GTest::gtest GTest::gtest_main)
    koral_engine_runtime(keditor_tests)
    target_compile_definitions(keditor_tests PRIVATE
        KGRAPH_TEST_SHADERS="${CMAKE_SOURCE_DIR}/modules/kgraph/tests/shaders"
        KGRAPH_PACKAGE_SHADERS="${CMAKE_SOURCE_DIR}/packages/koral/shaders"
        KEDITOR_TEST_SHADERS="${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders"
        KEDITOR_PROJECTS="${KORAL_ENGINE_PROJECTS}"
        # Forward+ made with nodes: its shaders, and where the project the test makes of it is left to be opened.
        KEDITOR_FORWARD_PLUS_SHADERS="${CMAKE_SOURCE_DIR}/samples/forward-plus/shaders"
        KEDITOR_FORWARD_PLUS_PROJECT="${CMAKE_BINARY_DIR}/forward-plus-project")
    # Freed memory is filled with a pattern, and a test that never ends is stopped: an edit that frees what a frame
    # is still drawing with reads what it freed, and shows as a crash or a wait that never ends rather than as luck.
    gtest_discover_tests(keditor_tests DISCOVERY_MODE PRE_TEST PROPERTIES ENVIRONMENT "MALLOC_PERTURB_=165" TIMEOUT 300)
endif()
