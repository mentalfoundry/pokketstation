add_executable(psemu_smoke_test smoke_test.c)
target_link_libraries(psemu_smoke_test PRIVATE psemu)
add_test(NAME smoke_test COMMAND psemu_smoke_test)

add_executable(psemu_cpu_test cpu_test.c)
target_include_directories(psemu_cpu_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(psemu_cpu_test PRIVATE psemu)
add_test(NAME cpu_test COMMAND psemu_cpu_test)

add_executable(psemu_ir_test ir_test.c)
target_include_directories(psemu_ir_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(psemu_ir_test PRIVATE psemu)
add_test(NAME ir_test COMMAND psemu_ir_test)

add_executable(psemu_com_test com_test.c)
target_include_directories(psemu_com_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(psemu_com_test PRIVATE psemu)
add_test(NAME com_test COMMAND psemu_com_test)
