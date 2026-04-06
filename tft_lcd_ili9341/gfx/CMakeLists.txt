add_library(gfx
	#gfx_adafruit.c
	gfx_ili9341.c
)

target_include_directories(gfx PUBLIC
	.
	${CMAKE_SOURCE_DIR}
)

target_link_libraries(gfx pico_stdlib hardware_spi hardware_dma ili9341)