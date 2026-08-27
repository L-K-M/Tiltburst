find_package(GTest CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_path(miniaudio_INCLUDE_DIR NAMES miniaudio.h REQUIRED)

add_executable(tb_tests
    core/config_test.cpp
    core/hash_test.cpp
    core/rng_test.cpp
    core/scaffold_test.cpp
    core/time_test.cpp
    audio/audio_test.cpp
    game/game_framework_test.cpp
    sim/det_replay_flipper_test.cpp
    platform/input_test.cpp
    sim/elements_test.cpp
    sim/lifecycle_test.cpp
    sim/ramp_test.cpp
    sim/script_test.cpp
    table/table_test.cpp
    render/font_assets_test.cpp
    render/projection_test.cpp
    render/render_smoke_test.cpp
    render/shader_build_test.cpp
    sim/broadphase_test.cpp
    sim/ccd_test.cpp
    sim/determinism_test.cpp
    sim/feel_test.cpp
    sim/golden_test.cpp
    sim/perf_and_layout_test.cpp
    sim/snapshot_test.cpp
    sim/solver_test.cpp
    sim/tick_loop_test.cpp
    support/alloc_hook.cpp
    support/tape.cpp
)

target_include_directories(tb_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${Stb_INCLUDE_DIR})
target_link_libraries(tb_tests PRIVATE
    tb_core tb_sim tb_platform tb_render tb_table tb_game tb_audio
    nlohmann_json::nlohmann_json GTest::gtest GTest::gtest_main)

include(GoogleTest)
# Repo-relative test data: tests resolve paths through tb::test::data_path(),
# never the process working directory (16-testing-ci.md §2).
target_compile_definitions(tb_tests PRIVATE
    TB_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    TB_SHADER_BUILD_DIR="${CMAKE_BINARY_DIR}/bin/shaders")

# Three discovery passes give per-class timeouts. Fallback if multiple passes
# on one target misbehave: a single pass with TIMEOUT 2400.
gtest_discover_tests(tb_tests DISCOVERY_MODE PRE_TEST
  TEST_FILTER "*-fuzz_sim_*:perf_*" PROPERTIES TIMEOUT 120)
gtest_discover_tests(tb_tests DISCOVERY_MODE PRE_TEST
  TEST_FILTER "fuzz_sim_*" PROPERTIES TIMEOUT 1800)
gtest_discover_tests(tb_tests DISCOVERY_MODE PRE_TEST
  TEST_FILTER "perf_*" PROPERTIES TIMEOUT 900)

# Tool readiness gate. tb_validate/tb_autoplay are usage-printing stubs from
# M0 (04-milestones.md M15 implements them), while /tables holds real packs
# from M5. Registering the per-table tool tests before M15 would turn every
# build-test job red for ten milestones, so registration is behind an option
# that is OFF until the M15 PR edits this line to ON.
option(TB_TOOLS_READY "Register per-table tb_validate/tb_autoplay tests" OFF)

if(TB_TOOLS_READY)
  file(GLOB table_jsons CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tables/*/table.json")
  foreach(tj ${table_jsons})
    get_filename_component(slug_dir "${tj}" DIRECTORY)
    get_filename_component(slug "${slug_dir}" NAME)

    add_test(NAME validate_${slug} COMMAND tb_validate ${slug_dir} --strict)
    set_tests_properties(validate_${slug} PROPERTIES TIMEOUT 120)

    add_test(NAME smoke_autoplay_${slug}
      COMMAND tb_autoplay ${slug_dir} --skill 1 --seconds 300
              --seed 7 --check-bounds)
    set_tests_properties(smoke_autoplay_${slug} PROPERTIES TIMEOUT 600)

    foreach(skill 0 1 2)
      add_test(NAME bounds_autoplay_s${skill}_${slug}
        COMMAND tb_autoplay ${slug_dir} --skill ${skill} --runs 20
                --seed 1 --balls 3 --check-bounds)
      set_tests_properties(bounds_autoplay_s${skill}_${slug}
        PROPERTIES TIMEOUT 900)
    endforeach()
  endforeach()
endif()

# Release gates (§2.10). Registered at M19; the frame-time gate is always
# registered and reports SKIPPED (exit 2) where no hardware GPU exists, so
# the gate can never be quietly deleted.
option(TB_RELEASE_GATES "Register the §2.10 release gates" OFF)  # ON at M19
if(TB_RELEASE_GATES)
  add_test(NAME perf_frame.gate_render_frame_time
    COMMAND ${CMAKE_COMMAND} -DTB_BIN=$<TARGET_FILE:tiltburst>
            -DTB_FRAMES=660 -DTB_WARMUP=60 -DTB_RUNS=3
            -P ${CMAKE_SOURCE_DIR}/tools/perf/frame_gate.cmake)
  set_tests_properties(perf_frame.gate_render_frame_time
    PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 2)
endif()

# Flake quarantine (§6), committed empty at M0. Read at CONFIGURE time,
# applied at TEST time via a generated include (see 16-testing-ci.md §2).
set(tb_quarantine "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/quarantine.txt")
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/quarantine.txt" tb_quarantine
       REGEX "^[^#[:space:]]")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               "${CMAKE_CURRENT_SOURCE_DIR}/quarantine.txt")
endif()
if(tb_quarantine)
  list(LENGTH tb_quarantine tb_quarantine_n)
  if(tb_quarantine_n GREATER 5)
    message(WARNING "quarantine.txt holds ${tb_quarantine_n} entries; §6 caps"
                    " further quarantines at 5 — deflake the oldest first")
  endif()
  set(tb_q_file "${CMAKE_CURRENT_BINARY_DIR}/tb_quarantine.cmake")
  file(WRITE "${tb_q_file}"
    "# generated from tests/quarantine.txt — do not edit\n"
    "if(NOT \"\$ENV{TB_RUN_QUARANTINED}\" STREQUAL \"1\")\n")
  foreach(tb_q_test ${tb_quarantine})
    string(STRIP "${tb_q_test}" tb_q_test)
    file(APPEND "${tb_q_file}"
      "  if(TEST ${tb_q_test})\n"
      "    set_tests_properties(${tb_q_test} PROPERTIES DISABLED TRUE)\n"
      "  endif()\n")
  endforeach()
  file(APPEND "${tb_q_file}" "endif()\n")
  set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${tb_q_file}")
  message(STATUS "Quarantined tests (16 §6): ${tb_quarantine}")
endif()
