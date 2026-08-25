find_package(SDL3 CONFIG REQUIRED)

add_library(tb_render STATIC
    render.cpp
)

target_link_libraries(tb_render PUBLIC tb_core tb_platform SDL3::SDL3)
