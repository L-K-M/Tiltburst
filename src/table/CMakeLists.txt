find_package(nlohmann_json CONFIG REQUIRED)

add_library(tb_table STATIC
    table.cpp
)

target_link_libraries(tb_table PUBLIC tb_core tb_sim nlohmann_json::nlohmann_json)
