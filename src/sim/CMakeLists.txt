# lua ships an unofficial-lua config target on this baseline (5.5.1,
# ADR-025). If a future baseline regresses to the FindLua-module port,
# swap to find_package(Lua REQUIRED) + LUA_LIBRARIES.
find_package(unofficial-lua CONFIG REQUIRED)
find_package(sol2 CONFIG REQUIRED)

# tb_sim depends only on tb_core (canon PLAN.md §5.1); enforced by
# Layout.SimIncludesNothingForbidden.
add_library(tb_sim STATIC
    math.cpp
    material.cpp
    ccd.cpp
    flipper.cpp
    flipper_ccd.cpp
    ramp.cpp
    script_host.cpp
    broadphase.cpp
    replay.cpp
    sim.cpp
    sim_thread.cpp
    solver.cpp
)

target_include_directories(tb_sim PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_sim PUBLIC tb_core unofficial::lua::lua sol2::sol2)

# Determinism rule 08 §2.2: no fast math, no FMA contraction drift.
if(MSVC)
    target_compile_options(tb_sim PRIVATE /fp:precise)
else()
    target_compile_options(tb_sim PRIVATE -ffp-contract=off)
endif()
