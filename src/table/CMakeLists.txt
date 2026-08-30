find_package(nlohmann_json CONFIG REQUIRED)

add_library(tb_table STATIC
    sim_builder.cpp
    table.cpp
    table_loader.cpp
    validator.cpp
)

target_include_directories(tb_table PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_table PUBLIC tb_core tb_sim tb_render tb_audio
                            nlohmann_json::nlohmann_json)
