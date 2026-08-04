include(FetchContent)
FetchContent_Declare(
    libretro-common
    GIT_REPOSITORY https://github.com/libretro/libretro-common.git
    GIT_TAG master
    SOURCE_SUBDIR nonexistent-headers-only # we only want the headers, not its own CMake build
)
FetchContent_MakeAvailable(libretro-common)

add_library(pokketstation_libretro MODULE libretro.c)
target_include_directories(pokketstation_libretro PRIVATE ${libretro-common_SOURCE_DIR}/include)
target_link_libraries(pokketstation_libretro PRIVATE psemu)
set_target_properties(pokketstation_libretro PROPERTIES
    PREFIX ""
    OUTPUT_NAME "pokketstation_libretro"
)

# CMake gives MODULE libraries a .so suffix on Apple, but libretro (and the
# osx-cmake CI templates, which move $BUILD_DIR/${CORENAME}_libretro.dylib)
# expect .dylib there. Without this the macOS build compiles and links
# cleanly and then fails on the move, which reads as a link failure but
# isn't. Every other platform's default suffix is already correct: .so on
# Linux/webOS, .dll on Windows, and the android template renames the plain
# .so to ${CORENAME}_libretro_android.so itself.
if(APPLE)
    set_target_properties(pokketstation_libretro PROPERTIES SUFFIX ".dylib")
endif()
