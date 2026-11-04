# Desktop harness for the portable Generator core (src/deluge/generative/core/).
# Builds standalone (cmake -S tests/generative_harness -B build/generative_harness) or as part of tests/.
cmake_minimum_required(VERSION 3.24)
project(GenerativeHarness CXX)

file(GLOB generative_core_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_LIST_DIR}/../../src/deluge/generative/core/*.cpp
)

add_executable(generative_harness main.cpp ${generative_core_SOURCES})
target_include_directories(generative_harness PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/deluge)
set_target_properties(generative_harness PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)
# The core must stay warning-free and build without exceptions or RTTI, as it does in the firmware.
target_compile_options(generative_harness PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
