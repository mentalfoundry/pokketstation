add_library(psemu STATIC
    src/arm_exec.c
    src/cpu.c
    src/flash.c
    src/io.c
    src/ir.c
    src/lcd.c
    src/memory.c
    src/psemu.c
    src/thumb_exec.c
    src/timer.c
)

target_include_directories(psemu
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(psemu PUBLIC c_std_99)

if(MSVC)
    target_compile_options(psemu PRIVATE /W4)
else()
    target_compile_options(psemu PRIVATE -Wall -Wextra)
endif()
