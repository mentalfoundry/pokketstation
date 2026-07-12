add_executable(psemu_inspect inspect.c)
target_include_directories(psemu_inspect PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(psemu_inspect PRIVATE psemu)

add_executable(frame_audio_probe frame_audio_probe.c)
target_include_directories(frame_audio_probe PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(frame_audio_probe PRIVATE psemu)
