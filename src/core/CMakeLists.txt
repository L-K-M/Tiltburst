find_package(fmt CONFIG REQUIRED)

add_library(tb_core STATIC
    core.cpp
    version.cpp
)

target_include_directories(tb_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_core PUBLIC fmt::fmt)
