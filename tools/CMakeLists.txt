add_executable(psemu_inspect inspect.c)
target_include_directories(psemu_inspect PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(psemu_inspect PRIVATE psemu)
