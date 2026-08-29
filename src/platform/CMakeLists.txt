find_package(SDL3 CONFIG REQUIRED)

# NOTE: app.cpp (the §1 boot/main-loop orchestration) belongs to the
# tiltburst executable target, not this library — executable-object
# symbols must be visible to the linker before static archives are
# scanned.
add_library(tb_platform STATIC
    backglass_pacer.cpp
    display_detect.cpp
    display_detect_sdl.cpp
    platform.cpp
    gpu_device.cpp
    input.cpp
    input_evdev.cpp
    input_sdl.cpp
    input_winraw.cpp
    latency.cpp
    paths.cpp
    window.cpp
)

target_include_directories(tb_platform PUBLIC ${CMAKE_SOURCE_DIR}/src)
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(tb_platform PUBLIC tb_core SDL3::SDL3)
# JSON is implementation-only (display_detect.cpp); PRIVATE keeps the
# detection header's purity honest for consumers.
target_link_libraries(tb_platform PRIVATE nlohmann_json::nlohmann_json)
