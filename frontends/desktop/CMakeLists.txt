find_package(SDL2 QUIET CONFIG)
if(NOT SDL2_FOUND)
    message(WARNING "SDL2 not found - skipping desktop frontend. Install SDL2 (e.g. `vcpkg install sdl2`) and reconfigure.")
    return()
endif()

add_executable(pokketstation_desktop main.c)
target_link_libraries(pokketstation_desktop PRIVATE psemu SDL2::SDL2 SDL2::SDL2main)
