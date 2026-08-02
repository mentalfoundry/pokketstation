add_executable(psemu_inspect inspect.c)
target_include_directories(psemu_inspect PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(psemu_inspect PRIVATE psemu)

add_executable(frame_audio_probe frame_audio_probe.c)
target_include_directories(frame_audio_probe PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(frame_audio_probe PRIVATE psemu)

add_executable(date_screen_probe date_screen_probe.c)
target_include_directories(date_screen_probe PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(date_screen_probe PRIVATE psemu)

# Links psemu_trace for the bus write-trace hook, which its "flashwatch" mode
# uses to tell an attempted flash write from one that never happened (see
# psemu_bus_write_trace_cb in core/src/memory.c).
add_executable(ir_probe ir_probe.c)
target_include_directories(ir_probe PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(ir_probe PRIVATE psemu_trace)

add_executable(bench_probe bench_probe.c)
target_include_directories(bench_probe PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(bench_probe PRIVATE psemu)

# Links plain psemu on purpose: it measures what frontends pay.
add_executable(core_bench core_bench.c)
target_include_directories(core_bench PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(core_bench PRIVATE psemu)

# These two need the bus read-trace hook, so they link psemu_trace rather
# than psemu (see core/CMakeLists.txt).
add_executable(volume_probe volume_probe.c)
target_include_directories(volume_probe PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(volume_probe PRIVATE psemu_trace)

add_executable(datetime_probe datetime_probe.c)
target_include_directories(datetime_probe PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(datetime_probe PRIVATE psemu_trace)

add_executable(button_timing_probe button_timing_probe.c)
target_include_directories(button_timing_probe PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(button_timing_probe PRIVATE psemu)

add_executable(pk_exit_test pk_exit_test.c)
target_include_directories(pk_exit_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(pk_exit_test PRIVATE psemu)
