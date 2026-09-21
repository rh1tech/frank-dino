# tapein — GP45 sampled into a bit buffer, so the Spectrum's own ROM can
# decode a real cassette. Ported from pico-speccy's LoadWavStream.
add_library(tapein INTERFACE)

target_sources(tapein INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/tapein.c
)

target_include_directories(tapein INTERFACE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(tapein INTERFACE
        dino_core
        pico_stdlib
        hardware_gpio
)
