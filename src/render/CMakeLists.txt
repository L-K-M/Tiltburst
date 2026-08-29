find_package(SDL3 CONFIG REQUIRED)
find_package(Stb REQUIRED)

add_library(tb_render STATIC
    backglass_renderer.cpp
    tbart.cpp
    particles.cpp
    font_atlas.cpp
    art_renderer.cpp
    bloom_chain.cpp
    render_renderer.cpp
    overlay.cpp
    pipeline_quad.cpp
    present_pass.cpp
    sdf_batch.cpp
    sdl_gpu_renderer.cpp
    shader_load.cpp
    view.cpp
)

target_include_directories(tb_render PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(tb_render PRIVATE ${Stb_INCLUDE_DIR})
# Dev-machine fallback for shader blobs when running from a build tree
# whose layout separates executables from the blob install dir.
if(NOT TB_SHADER_FALLBACK_DIR)
    set(TB_SHADER_FALLBACK_DIR "${CMAKE_SOURCE_DIR}/shaders/compiled")
endif()
target_compile_definitions(tb_render PRIVATE
    TB_SHADER_FALLBACK_DIR="${TB_SHADER_FALLBACK_DIR}")

target_link_libraries(tb_render PUBLIC tb_core tb_platform SDL3::SDL3)
