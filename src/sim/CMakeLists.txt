# tb_sim depends only on tb_core (canon PLAN.md §5.1); enforced by
# Layout.SimIncludesNothingForbidden from M2.
add_library(tb_sim STATIC
    sim.cpp
    sim_thread.cpp
)

target_include_directories(tb_sim PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_sim PUBLIC tb_core)
