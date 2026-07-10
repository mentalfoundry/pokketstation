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
