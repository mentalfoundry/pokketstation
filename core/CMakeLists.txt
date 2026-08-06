set(PSEMU_SOURCES
    src/arm_exec.c
    src/clk.c
    src/com.c
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
    src/state.c
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

    # The libretro core is a shared module that links these static libraries into itself. On any target
    # whose ABI forbids absolute relocations inside a shared object, a non-PIC static library cannot go
    # into one at all - the link fails outright with "relocation R_X86_64_32 ... recompile with -fPIC".
    # This built fine on most CI targets for reasons that are all accidental rather than portable: the
    # Android NDK and Apple's clang default to PIC, aarch64 addressing is PC-relative anyway, and 32-bit
    # x86 tolerates text relocations. Only x86_64 Linux and 32-bit ARM webOS actually rejected it, so
    # relying on those defaults meant two platforms breaking while the rest looked healthy. Ask for it.
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endforeach()
