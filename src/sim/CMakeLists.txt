#vcpkg 's lua 5.4.x port is consumed through CMake' s FindLua module
#(unlike 5.5.x, which shipped an unofficial - lua config target).
find_package(Lua 5.4 REQUIRED) find_package(sol2 CONFIG REQUIRED)

#tb_sim depends only on tb_core(canon PLAN.md §5.1); enforced by
#Layout.SimIncludesNothingForbidden.
    add_library(tb_sim STATIC
                    math.cpp material.cpp ccd.cpp flipper.cpp flipper_ccd.cpp ramp.cpp script_host
                        .cpp broadphase.cpp replay.cpp sim.cpp sim_thread.cpp solver.cpp)

        target_include_directories(tb_sim PUBLIC ${CMAKE_SOURCE_DIR} / src ${LUA_INCLUDE_DIR})
            target_link_libraries(tb_sim PUBLIC tb_core ${LUA_LIBRARIES} sol2::sol2)

#Determinism rule 08 §2.2 : no fast math, no FMA contraction drift.
                if (MSVC) target_compile_options(tb_sim PRIVATE / fp
                                                 : precise) else()
                    target_compile_options(tb_sim PRIVATE - ffp - contract = off) endif()
