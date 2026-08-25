# miniaudio is header-only and ships no CMake config; resolve its include
# dir through the toolchain (works under vcpkg manifest mode).
find_path(miniaudio_INCLUDE_DIR NAMES miniaudio.h REQUIRED)

if(NOT TARGET miniaudio::miniaudio)
    add_library(miniaudio::miniaudio INTERFACE IMPORTED)
    set_target_properties(miniaudio::miniaudio PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${miniaudio_INCLUDE_DIR}")
endif()

add_library(tb_audio STATIC
    audio.cpp
)

target_link_libraries(tb_audio PUBLIC tb_core miniaudio::miniaudio)
