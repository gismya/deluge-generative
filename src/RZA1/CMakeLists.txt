file(GLOB_RECURSE RZA1_SOURCES CONFIGURE_DEPENDS *.S *.c *.h)

add_library(RZA1 STATIC ${RZA1_SOURCES})
target_include_directories(RZA1 PRIVATE ${SHARED_INCLUDE})
target_link_libraries(RZA1 PRIVATE fatfs)

target_compile_definitions(RZA1 PRIVATE
    $<$<CONFIG:DEBUG>:ENABLE_TEXT_OUTPUT=1>
)

if(ENABLE_RTT)
  message(STATUS "RTT enabled for RZA1")

  target_link_libraries(RZA1 PRIVATE RTT)

  target_compile_definitions(RZA1 PRIVATE
      $<$<CONFIG:DEBUG>:HAVE_RTT=1>
  )
endif(ENABLE_RTT)

if(ENABLE_SYSEX_TEST)
  # The stand-in hardware probes hook the OLED driver (src/deluge/generative/debug/hw_probe.h).
  target_compile_definitions(RZA1 PRIVATE ENABLE_SYSEX_TEST=1)
endif(ENABLE_SYSEX_TEST)
