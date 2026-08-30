find_package(nlohmann_json CONFIG REQUIRED)

add_executable(tb_validate tb_validate_main.cpp)
add_executable(tb_autoplay tb_autoplay_main.cpp)
add_executable(tb_screenshot tb_screenshot_main.cpp)

# From M15 the tools link the public APIs of their modules only
# (04-milestones.md M15 "Key interfaces"). The validator walks table +
# art + audio; autoplay drives the sim; screenshot renders.
target_link_libraries(tb_validate PRIVATE tb_table tb_render tb_audio tb_sim tb_core)
target_link_libraries(tb_autoplay PRIVATE tb_table tb_sim tb_game tb_core
                                         nlohmann_json::nlohmann_json)
target_link_libraries(tb_screenshot PRIVATE tb_table tb_sim tb_render tb_platform tb_core)
