add_executable(tb_validate tb_validate_main.cpp)
add_executable(tb_autoplay tb_autoplay_main.cpp)
add_executable(tb_screenshot tb_screenshot_main.cpp)

# From M15 the tools link the public APIs of their modules only
# (04-milestones.md M15 "Key interfaces"); stubs need nothing.
