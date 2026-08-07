# SPDX-FileCopyrightText: Copyright (c) 2026 Darien Liu (mentalfoundry)
# SPDX-License-Identifier: MIT

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

# The memory card protocol at the connector. This suite needs a BIOS dump, because the kernel of the
# emulated machine holds the protocol. It skips itself with exit code 77 when it finds no dump.
#
# THIS TARGET GETS NO PATH TO core/src. A PS1 emulator gets the public header and no more, and
# tests/mock_ps1.c is the reference for such a port. Without this limit, that module could use a
# private header, and a gap in the public header would then stay hidden.
add_executable(psemu_bu_test bu_test.c mock_ps1.c)
target_include_directories(psemu_bu_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(psemu_bu_test PRIVATE PSEMU_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/testdata")
target_link_libraries(psemu_bu_test PRIVATE psemu)
add_test(NAME bu_test COMMAND psemu_bu_test)
set_tests_properties(bu_test PROPERTIES SKIP_RETURN_CODE 77)
