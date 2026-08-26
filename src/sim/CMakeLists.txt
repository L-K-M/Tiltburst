# tb_sim depends only on tb_core (canon PLAN.md §5.1); enforced by
# Layout.SimIncludesNothingForbidden.
add_library(tb_sim STATIC
    math.cpp
    material.cpp
    ccd.cpp
    flipper.cpp
    flipper_ccd.cpp
    ramp.cpp
    broadphase.cpp
    replay.cpp
    sim.cpp
    sim_thread.cpp
    solver.cpp
)

target_include_directories(tb_sim PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_sim PUBLIC tb_core)

# Determinism rule 08 §2.2: no fast math, no FMA contraction drift.
if(MSVC)
    target_compile_options(tb_sim PRIVATE /fp:precise)
else()
    target_compile_options(tb_sim PRIVATE -ffp-contract=off)
endif()
