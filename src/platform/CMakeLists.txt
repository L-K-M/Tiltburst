find_package(SDL3 CONFIG REQUIRED)

add_library(tb_platform STATIC
    platform.cpp
)

target_link_libraries(tb_platform PUBLIC tb_core SDL3::SDL3)
