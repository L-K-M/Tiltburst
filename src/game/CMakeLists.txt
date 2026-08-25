add_library(tb_game STATIC
    game.cpp
)

target_link_libraries(tb_game PUBLIC tb_core tb_sim tb_table)
