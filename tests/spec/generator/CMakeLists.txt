# The Generator's harness: its CppSpec *_spec.cpp files, one ctest per file (spec §21.2).
#
#   cmake -S tests -B build/tests -G Ninja [-DGENERATOR_SPECS_SANITIZE=ON]
#   cmake --build build/tests --target generator_specs
#   ctest --test-dir build/tests -L generator --output-on-failure
#
# CI's x86 job turns GENERATOR_SPECS_SANITIZE on, so a signed overflow fails the run (ADR 0004).
# The QEMU job doesn't build this target.

option(GENERATOR_SPECS_SANITIZE "Abort the Generator specs on signed integer overflow" OFF)

# Build flags for the Generator's code and its specs. generator_core links this too, so its sources build with
# the same flags as the specs that test it.
add_library(generator_build_flags INTERFACE)

# Plain char is signed in the firmware build, so it must be here too (ADR 0004).
target_compile_options(generator_build_flags INTERFACE -fsigned-char)

if (GENERATOR_SPECS_SANITIZE)
  set(generator_sanitize_flags
    -fsanitize=signed-integer-overflow
    -fno-sanitize-recover=signed-integer-overflow
  )
  target_compile_options(generator_build_flags INTERFACE ${generator_sanitize_flags})
  target_link_options(generator_build_flags INTERFACE ${generator_sanitize_flags})
endif ()

# The portable Generator core and its portable drivers: the same sources the firmware builds (spec §2.1, §2.5).
# generative/firmware/ holds the firmware adapters and stays out.
set(generative_dir ${CMAKE_CURRENT_LIST_DIR}/../../../src/deluge/generative)
file(GLOB_RECURSE generator_core_sources CONFIGURE_DEPENDS
  ${generative_dir}/core/*.cpp
  ${generative_dir}/perform/*.cpp
)
add_library(generator_core STATIC ${generator_core_sources})
target_include_directories(generator_core PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../../../src/deluge)
target_link_libraries(generator_core PUBLIC generator_build_flags)
set_target_properties(generator_core PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED YES
)

create_specs_driver(generator_specs ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(generator_specs PRIVATE generator_core)

get_property(generator_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${generator_tests} PROPERTIES LABELS generator)

# The rough render (spec §21.2): our own small voices and file writers over the harness, and the command that renders
# a Recipe to a WAV, a MIDI file and a piano roll. Not specs, so outside the *_spec.cpp glob.
file(GLOB generator_render_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/render/*.cpp)
list(FILTER generator_render_sources EXCLUDE REGEX ".*/render_main\\.cpp$")
add_library(generator_render STATIC ${generator_render_sources})
target_include_directories(generator_render PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(generator_render PUBLIC generator_core)
# Where the command finds pack/ by default.
target_compile_definitions(generator_render PRIVATE GENERATOR_RENDER_REPO_ROOT="${CMAKE_CURRENT_LIST_DIR}/../../..")
set_target_properties(generator_render PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED YES
)
target_link_libraries(generator_specs PRIVATE generator_render)

add_executable(generator_render_cli ${CMAKE_CURRENT_LIST_DIR}/render/render_main.cpp)
set_target_properties(generator_render_cli PROPERTIES
  OUTPUT_NAME generator_render
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED YES
)
target_link_libraries(generator_render_cli PRIVATE generator_render)
