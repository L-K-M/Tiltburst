find_package(SDL3 CONFIG REQUIRED)

add_library(tb_platform STATIC
    platform.cpp
    app.cpp
    gpu_device.cpp
    paths.cpp
    window.cpp
)

target_include_directories(tb_platform PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_platform PUBLIC tb_core SDL3::SDL3)
