add_executable(pico_emb main.c)

set_target_properties(pico_emb PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

target_include_directories(pico_emb PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(pico_emb
    pico_stdlib
    hardware_gpio
    hardware_pwm
    hardware_timer
    hardware_clocks
    hardware_spi
    ili9341
    gfx
)

pico_add_extra_outputs(pico_emb)