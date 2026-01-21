cmake_minimum_required(VERSION 3.13)

option(PICOUSBKEYBRIDGE_FORCE_RP2350_DEFAULT "Force RP2350 defaults if unset or rp2040" ON)
if (PICOUSBKEYBRIDGE_FORCE_RP2350_DEFAULT)
  if (NOT DEFINED PICO_PLATFORM OR PICO_PLATFORM STREQUAL "" OR PICO_PLATFORM STREQUAL "rp2040")
    set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Pico platform" FORCE)
  endif ()
  if (NOT DEFINED PICO_BOARD OR PICO_BOARD STREQUAL "" OR PICO_BOARD STREQUAL "pico")
    set(PICO_BOARD pico2 CACHE STRING "Pico board" FORCE)
  endif ()
else ()
  if (NOT PICO_PLATFORM)
    set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Pico platform")
  endif ()
  if (NOT PICO_BOARD)
    set(PICO_BOARD pico2 CACHE STRING "Pico board")
  endif ()
endif ()

if (NOT PICO_SDK_PATH AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/third_party/pico-sdk")
  set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/third_party/pico-sdk")
endif ()

include(cmake/pico_sdk_import.cmake)

project(PicoUSBKeyBridge C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

set(PICO_PIO_USB_PATH ${CMAKE_CURRENT_LIST_DIR}/third_party/pico-pio-usb)
add_subdirectory(${PICO_PIO_USB_PATH} pico_pio_usb)

add_executable(PicoUSBKeyBridge
  src/log.c
  src/main.c
  src/usb_descriptors.c
)

target_include_directories(PicoUSBKeyBridge PRIVATE src)
target_link_libraries(PicoUSBKeyBridge PRIVATE
  pico_stdlib
  pico_multicore
  hardware_pio
  hardware_dma
  hardware_watchdog
  tinyusb_device
  pico_pio_usb
)

pico_enable_stdio_uart(PicoUSBKeyBridge 0)
pico_enable_stdio_usb(PicoUSBKeyBridge 0)
pico_add_extra_outputs(PicoUSBKeyBridge)
