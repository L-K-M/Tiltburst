cmake_minimum_required(VERSION 3.28)
project(tiltburst VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# No C++20 modules anywhere in Tiltburst; skip clang-scan-deps passes.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
# Executables and the shader blob dir colocate (06-rendering.md §16.4).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_compile_definitions($<$<CONFIG:Debug>:TB_DEBUG>)

# Static libs: tb_core tb_platform tb_sim tb_render tb_audio tb_table tb_game
add_subdirectory(src/core)      # links: fmt
add_subdirectory(src/platform)  # links: tb_core, SDL3
add_subdirectory(src/sim)       # links: tb_core ONLY (canon PLAN.md §5.1)
add_subdirectory(src/render)    # links: tb_core, tb_platform, SDL3
add_subdirectory(src/audio)     # links: tb_core, miniaudio
add_subdirectory(src/table)     # links: tb_core, tb_sim, nlohmann_json
add_subdirectory(src/game)      # links: tb_core, tb_sim, tb_table

# app.cpp orchestrates every subsystem, so it compiles as an executable
# object: its undefined symbols exist before any static archive is scanned
# (GNU ld single-pass archive semantics).
add_executable(tiltburst
    src/main.cpp
    src/platform/app.cpp
)
target_link_libraries(tiltburst PRIVATE tb_render tb_audio tb_game)
add_subdirectory(src/tools)     # tb_validate, tb_autoplay, tb_screenshot (canon §5.1)

include(${CMAKE_SOURCE_DIR}/cmake/shaders.cmake)

enable_testing()
add_subdirectory(tests)         # tb_tests (gtest)
