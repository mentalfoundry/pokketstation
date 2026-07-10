cmake_minimum_required(VERSION 3.16)
project(pokketstation C)

option(PSEMU_BUILD_LIBRETRO "Build the libretro core" ON)
option(PSEMU_BUILD_DESKTOP "Build the SDL2 desktop frontend" ON)
option(PSEMU_BUILD_TESTS "Build the smoke test" ON)

add_subdirectory(core)

if(PSEMU_BUILD_LIBRETRO)
    add_subdirectory(frontends/libretro)
endif()

if(PSEMU_BUILD_DESKTOP)
    add_subdirectory(frontends/desktop)
endif()

if(PSEMU_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# frontends/vita is only buildable when configured with the vitasdk
# toolchain file (-DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake);
# its own CMakeLists.txt no-ops out otherwise, so it's always added here.
add_subdirectory(frontends/vita)
