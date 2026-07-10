if(NOT CMAKE_SYSTEM_NAME STREQUAL "Vita")
    message(STATUS "Skipping Vita frontend (configure with the vitasdk toolchain file to build it, see frontends/vita/README.md)")
    return()
endif()

include("${VITASDK}/share/vita.cmake" REQUIRED)

add_executable(pokketstation_vita main.c)
target_link_libraries(pokketstation_vita PRIVATE
    psemu
    vita2d
    SceDisplay_stub
    SceCtrl_stub
    SceGxm_stub
    SceSysmodule_stub
    SceCommonDialog_stub
)

vita_create_self(pokketstation_vita.self pokketstation_vita)
vita_create_vpk(pokketstation_vita.vpk "POKKETST01" pokketstation_vita.self
    VERSION "01.00"
    NAME "PokketStation"
)
