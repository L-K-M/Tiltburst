# tb_game (11-game-framework.md): the game state machine, players,
# tilt policy, scoring ledger presentation, and high scores. M10/M18.
add_library(tb_game STATIC
    game_machine.cpp
    high_scores.cpp
    score_format.cpp
)

target_include_directories(tb_game PUBLIC ${CMAKE_SOURCE_DIR}/src)
find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(tb_game PUBLIC tb_core tb_sim tb_table nlohmann_json::nlohmann_json)
