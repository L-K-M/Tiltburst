# miniaudio is header-only and ships no CMake config; resolve its include
# dir through the toolchain (works under vcpkg manifest mode).
find_path(miniaudio_INCLUDE_DIR NAMES miniaudio.h REQUIRED)

if(NOT TARGET miniaudio::miniaudio)
    add_library(miniaudio::miniaudio INTERFACE IMPORTED)
    set_target_properties(miniaudio::miniaudio PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${miniaudio_INCLUDE_DIR}")
endif()

find_package(nlohmann_json CONFIG REQUIRED)

add_library(tb_audio STATIC
    audio.cpp
    miniaudio_impl.c
    audio_bank.cpp
    audio_engine.cpp
    audio_json.cpp
    sfx_synth.cpp
    tracker.cpp
)

target_include_directories(tb_audio PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(tb_audio PUBLIC tb_core nlohmann_json::nlohmann_json
                                      miniaudio::miniaudio)

# Determinism (12-audio.md §8): the tracker's golden-hash PCM must be
# bit-identical across platforms — no FMA contraction drift (the same
# rule tb_sim carries, 08 §2.2).
if(MSVC)
    target_compile_options(tb_audio PRIVATE /fp:precise)
else()
    target_compile_options(tb_audio PRIVATE -ffp-contract=off)
endif()
