# FFT module
add_library(usermod_fft_core1 INTERFACE)
target_sources(usermod_fft_core1 INTERFACE ${CMAKE_CURRENT_LIST_DIR}/fft_core1/fft_core1.c)
target_include_directories(usermod_fft_core1 INTERFACE ${CMAKE_CURRENT_LIST_DIR}/fft_core1)
target_link_libraries(usermod INTERFACE usermod_fft_core1)

# IR module
add_library(usermod_ir_core1 INTERFACE)
target_sources(usermod_ir_core1 INTERFACE ${CMAKE_CURRENT_LIST_DIR}/ir_core1/ir_core1.c)
target_include_directories(usermod_ir_core1 INTERFACE ${CMAKE_CURRENT_LIST_DIR}/ir_core1)
target_link_libraries(usermod INTERFACE usermod_ir_core1)

# Aleds RGB module
add_library(usermod_aleds_rgb INTERFACE)
target_sources(usermod_aleds_rgb INTERFACE ${CMAKE_CURRENT_LIST_DIR}/aleds_rgb/aleds_rgb.c)
target_include_directories(usermod_aleds_rgb INTERFACE ${CMAKE_CURRENT_LIST_DIR}/aleds_rgb)
target_link_libraries(usermod INTERFACE usermod_aleds_rgb)

# LED Display module
add_library(usermod_leddisplay INTERFACE)
target_sources(usermod_leddisplay INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/leddisplay/leddisplay.c
)
target_include_directories(usermod_leddisplay INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/leddisplay
)
target_link_libraries(usermod INTERFACE usermod_leddisplay)