cmake_minimum_required(VERSION 3.16)
project(pokketstation C)

# The libretro buildbot's shared CI templates (see gitlab-ci.yml) grab the
# built core from a fixed, flat path under the build tree ($BUILD_DIR,
# optionally + $EXTRA_PATH) - without this, single-config generators
# (Linux/macOS/Android/webOS) would bury pokketstation_libretro.{so,dylib}
# under frontends/libretro/ instead, since that's the add_subdirectory()
# that actually defines the target. Multi-config generators (the Windows
# MSVC template) still append their own Release/Debug subfolder on top of
# this regardless - that's handled with EXTRA_PATH in gitlab-ci.yml
# instead, not here.
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

option(PSEMU_BUILD_LIBRETRO "Build the libretro core" ON)
option(PSEMU_BUILD_DESKTOP "Build the SDL2 desktop frontend" ON)
option(PSEMU_BUILD_TESTS "Build the smoke test" ON)

add_subdirectory(core)
add_subdirectory(tools)

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
