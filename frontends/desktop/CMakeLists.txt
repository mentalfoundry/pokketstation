# The desktop frontend's menu bar (main.c) is native Win32 (RC-defined menu
# and dialog, GetOpenFileNameA, message-hook-based WM_COMMAND handling) -
# rather than spread #ifdef __WIN32__ plumbing through main.c for a menu bar
# only one platform has, "desktop" is scoped to mean Windows for now. A
# different OS's desktop frontend, if one is ever added, belongs in its own
# frontends/ subdirectory instead.
if(NOT WIN32)
    message(WARNING "The desktop frontend currently targets Windows only - skipping.")
    return()
endif()

find_package(SDL2 QUIET CONFIG)
if(NOT SDL2_FOUND)
    message(WARNING "SDL2 not found - skipping desktop frontend. Install SDL2 (e.g. `vcpkg install sdl2`) and reconfigure.")
    return()
endif()

enable_language(RC)

# WIN32 here means "GUI subsystem" (CMake's name for /SUBSYSTEM:WINDOWS),
# not "this OS" - it's what keeps a console window from popping up
# alongside the SDL window by default. main() still gets a normal argc/argv
# via SDL2::SDL2main's WinMain shim; pass --console to opt back into a
# visible console for stderr diagnostics (see main.c).
add_executable(pokketstation_desktop WIN32 main.c resource.rc)
# comctl32 - the About dialog's SysLink control (Help > About's clickable
# repo link); shell32 - ShellExecuteA, to actually open it in a browser.
target_link_libraries(pokketstation_desktop PRIVATE psemu SDL2::SDL2 SDL2::SDL2main comdlg32 comctl32 shell32)

# CMake target name stays pokketstation_desktop (matches the
# pokketstation_libretro/pokketstation_vita naming convention and anything
# already scripted against --target pokketstation_desktop) - only the
# produced binary's filename changes, to plain pokketstation.exe.
set_target_properties(pokketstation_desktop PROPERTIES OUTPUT_NAME "pokketstation")
