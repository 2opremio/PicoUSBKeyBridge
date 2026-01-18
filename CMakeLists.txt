cmake_minimum_required(VERSION 3.13)

if (NOT PICO_SDK_PATH AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/third_party/pico-sdk")
  set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/third_party/pico-sdk")
endif ()

include(cmake/pico_sdk_import.cmake)

if (NOT PICO_PLATFORM)
  set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Pico platform")
endif ()
if (NOT PICO_BOARD)
  set(PICO_BOARD pico2 CACHE STRING "Pico board")
endif ()

project(keyemu C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

set(PICO_PIO_USB_PATH ${CMAKE_CURRENT_LIST_DIR}/third_party/pico-pio-usb)
add_subdirectory(${PICO_PIO_USB_PATH} pico_pio_usb)

add_executable(keyemu
  src/main.c
  src/usb_descriptors.c
)

target_include_directories(keyemu PRIVATE src)
target_link_libraries(keyemu PRIVATE
  pico_stdlib
  pico_multicore
  hardware_pio
  hardware_dma
  tinyusb_device
  pico_pio_usb
)

pico_enable_stdio_uart(keyemu 0)
pico_enable_stdio_usb(keyemu 0)
pico_add_extra_outputs(keyemu)
