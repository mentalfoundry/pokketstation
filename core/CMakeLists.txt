set(PSEMU_SOURCES
    src/arm_exec.c
    src/clk.c
    src/cpu.c
    src/dac.c
    src/flash.c
    src/intc.c
    src/iop.c
    src/ir.c
    src/lcd.c
    src/memory.c
    src/psemu.c
    src/rtc.c
    src/thumb_exec.c
    src/timer.c
)

add_library(psemu STATIC ${PSEMU_SOURCES})

# Same sources, built with the bus read-trace hook compiled in (see
# psemu_bus_read_trace_cb in src/memory.c). That hook sits on the hottest
# path in the emulator - one indirect-call check per byte of every bus
# read, including opcode fetch - and measured ~20% slower on a fixed 6.5M
# instruction workload. Frontends link `psemu` and pay nothing; the
# diagnostic tools in tools/ link `psemu_trace` and pay for what they use.
add_library(psemu_trace STATIC ${PSEMU_SOURCES})
target_compile_definitions(psemu_trace PUBLIC PSEMU_TRACE_HOOKS)

foreach(target psemu psemu_trace)
    target_include_directories(${target}
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_compile_features(${target} PUBLIC c_std_99)

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endforeach()
