# Shader build path (06-rendering.md §16.4, ADR-012).
#
# TB_COMPILE_SHADERS (default ON) compiles /shaders/*.hlsl to
# <binary>/bin/shaders/<name>.{spv,dxil,msl} through the SDL_shadercross
# CLI (FetchContent, pinned commit — upstream ships no release tags).
# When the toolchain cannot be provisioned on this platform (no DXC), the
# option is forced OFF with a warning and the committed blobs under
# /shaders/compiled are installed verbatim instead (ADR-012).

if(NOT DEFINED TB_SHADER_OUT_DIR)
    set(TB_SHADER_OUT_DIR ${CMAKE_BINARY_DIR}/bin/shaders)
endif()
set(TB_SHADER_SOURCE_DIR ${CMAKE_SOURCE_DIR}/shaders)

option(TB_COMPILE_SHADERS "Compile HLSL via SDL_shadercross at build time"
       ON)

set(TB_SHADER_BLOBS "")

if(TB_COMPILE_SHADERS)
    # The CLI needs SDL3 (headers + shared lib) and SPIRV-Cross with the C
    # API; DXC must be provided via TB_DXC_ROOT. Probe before fetching so a
    # missing toolchain degrades to blobs instead of failing configure.
    find_package(spirv_cross_c QUIET)
    set(TB_SHADERCROSS_DEPS_OK OFF)
    if(TARGET spirv_cross_c)
        set(TB_SHADERCROSS_DEPS_OK ON)
    endif()

    if(NOT TB_SHADERCROSS_DEPS_OK)
        message(WARNING
            "SDL_shadercross deps not found (spirv_cross_c); forcing "
            "TB_COMPILE_SHADERS=OFF per ADR-012 — installing committed "
            "blobs from shaders/compiled instead")
        set(TB_COMPILE_SHADERS OFF)
    endif()
endif()

if(TB_COMPILE_SHADERS)
    include(FetchContent)
    FetchContent_Declare(
        sdl_shadercross
        GIT_REPOSITORY https://github.com/libsdl-org/SDL_shadercross
        GIT_TAG e55cf5e31ced6f3d1be5cc6d0c50e99384f9f4ba
    )
    set(SDLSHADERCROSS_VENDORED OFF CACHE BOOL "" FORCE)
    set(SDLSHADERCROSS_SHARED OFF CACHE BOOL "" FORCE)
    set(SDLSHADERCROSS_STATIC ON CACHE BOOL "" FORCE)
    set(SDLSHADERCROSS_CLI ON CACHE BOOL "" FORCE)
    set(SDLSHADERCROSS_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(sdl_shadercross)

    foreach(name IN ITEMS sprite.vert sprite.frag sdf.vert sdf.frag present.vert present.frag bloom.vert bloom_bright.frag bloom_down.frag bloom_blur.frag bloom_up.frag)
        set(src ${TB_SHADER_SOURCE_DIR}/${name}.hlsl)
        add_custom_command(
            OUTPUT ${TB_SHADER_OUT_DIR}/${name}.spv
            COMMAND ${CMAKE_COMMAND} -E make_directory ${TB_SHADER_OUT_DIR}
            COMMAND shadercross ${src} -o ${TB_SHADER_OUT_DIR}/${name}.spv
            DEPENDS ${src}
        )
        list(APPEND TB_SHADER_BLOBS ${TB_SHADER_OUT_DIR}/${name}.spv)

        if(TB_DXC_ROOT)
            add_custom_command(
                OUTPUT ${TB_SHADER_OUT_DIR}/${name}.dxil
                COMMAND ${CMAKE_COMMAND} -E make_directory
                        ${TB_SHADER_OUT_DIR}
                COMMAND shadercross ${src} -o ${TB_SHADER_OUT_DIR}/${name}.dxil
                DEPENDS ${src}
            )
            list(APPEND TB_SHADER_BLOBS ${TB_SHADER_OUT_DIR}/${name}.dxil)
        endif()

        add_custom_command(
            OUTPUT ${TB_SHADER_OUT_DIR}/${name}.msl
            COMMAND ${CMAKE_COMMAND} -E make_directory ${TB_SHADER_OUT_DIR}
            COMMAND shadercross ${src} -o ${TB_SHADER_OUT_DIR}/${name}.msl
            DEPENDS ${src}
        )
        list(APPEND TB_SHADER_BLOBS ${TB_SHADER_OUT_DIR}/${name}.msl)
    endforeach()

    add_custom_target(tb_shaders ALL DEPENDS ${TB_SHADER_BLOBS})
else()
    # ADR-012 fallback: install the committed blobs verbatim.
    file(GLOB tb_committed_blobs CONFIGURE_DEPENDS
         ${TB_SHADER_SOURCE_DIR}/compiled/*)
    foreach(blob ${tb_committed_blobs})
        get_filename_component(bname ${blob} NAME)
        add_custom_command(
            OUTPUT ${TB_SHADER_OUT_DIR}/${bname}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${TB_SHADER_OUT_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${blob}
                    ${TB_SHADER_OUT_DIR}/${bname}
            DEPENDS ${blob}
        )
        list(APPEND TB_SHADER_BLOBS ${TB_SHADER_OUT_DIR}/${bname})
    endforeach()
    add_custom_target(tb_shaders ALL DEPENDS ${TB_SHADER_BLOBS})
endif()

# Every executable target depends on the blob set landing next to it.
foreach(tgt tiltburst tb_validate tb_autoplay tb_screenshot)
    if(TARGET ${tgt})
        add_dependencies(${tgt} tb_shaders)
    endif()
endforeach()
