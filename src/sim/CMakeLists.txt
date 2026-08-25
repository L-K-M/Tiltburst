# tb_sim depends only on tb_core (canon PLAN.md §5.1); enforced by
# Layout.SimIncludesNothingForbidden from M2.
add_library(tb_sim STATIC
    sim.cpp
)

target_link_libraries(tb_sim PUBLIC tb_core)
