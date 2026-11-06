# The Generator's harness: its CppSpec *_spec.cpp files, one ctest per file (spec §21.2).
#
#   cmake -S tests -B build/tests -G Ninja [-DGENERATOR_SPECS_SANITIZE=ON]
#   cmake --build build/tests --target generator_specs
#   ctest --test-dir build/tests -L generator --output-on-failure
#
# CI's x86 job turns GENERATOR_SPECS_SANITIZE on, so a signed overflow fails the run (ADR 0004).
# The QEMU job doesn't build this target.

option(GENERATOR_SPECS_SANITIZE "Abort the Generator specs on signed integer overflow" OFF)

create_specs_driver(generator_specs ${CMAKE_CURRENT_LIST_DIR})

# Plain char is signed in the firmware build, so it must be here too (ADR 0004).
target_compile_options(generator_specs PRIVATE -fsigned-char)

if (GENERATOR_SPECS_SANITIZE)
  set(generator_sanitize_flags
    -fsanitize=signed-integer-overflow
    -fno-sanitize-recover=signed-integer-overflow
  )
  target_compile_options(generator_specs PRIVATE ${generator_sanitize_flags})
  target_link_options(generator_specs PRIVATE ${generator_sanitize_flags})
endif ()

get_property(generator_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${generator_tests} PROPERTIES LABELS generator)
