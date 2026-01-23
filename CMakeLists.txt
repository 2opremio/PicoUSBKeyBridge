cmake_minimum_required(VERSION 3.13)

option(PICOUSBKEYBRIDGE_FORCE_RP2350_DEFAULT "Force RP2350 defaults if unset or rp2040" OFF)
if (PICOUSBKEYBRIDGE_FORCE_RP2350_DEFAULT)
  if (NOT DEFINED PICO_PLATFORM OR PICO_PLATFORM STREQUAL "" OR PICO_PLATFORM STREQUAL "rp2040")
    set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Pico platform")
  endif ()
  if (NOT DEFINED PICO_BOARD OR PICO_BOARD STREQUAL "" OR PICO_BOARD STREQUAL "pico" OR PICO_BOARD STREQUAL "pico2")
    set(PICO_BOARD waveshare_rp2350_usb_a CACHE STRING "Pico board")
  endif ()
else ()
  if (NOT PICO_PLATFORM)
    set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Pico platform")
  endif ()
  if (NOT PICO_BOARD)
    set(PICO_BOARD waveshare_rp2350_usb_a CACHE STRING "Pico board")
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

set(PUSBKB_UART_INDEX "1" CACHE STRING "UART instance index (0 or 1)")
set(PUSBKB_UART_BAUDRATE "115200" CACHE STRING "UART baud rate")
set(PUSBKB_UART_TX_PIN "4" CACHE STRING "UART TX GPIO pin")
set(PUSBKB_UART_RX_PIN "5" CACHE STRING "UART RX GPIO pin")
option(PUSBKB_HID_TEST "Enable USB-C HID test mode" OFF)

execute_process(
  COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
  RESULT_VARIABLE PUSBKB_GIT_RESULT
  OUTPUT_VARIABLE PUSBKB_GIT_COMMIT
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if (NOT PUSBKB_GIT_RESULT EQUAL 0)
  set(PUSBKB_GIT_COMMIT "unknown")
endif ()

add_executable(PicoUSBKeyBridge
  src/log.c
  src/main.c
  src/usb_descriptors.c
)

target_include_directories(PicoUSBKeyBridge PRIVATE src)
target_link_libraries(PicoUSBKeyBridge PRIVATE
  pico_stdlib
  hardware_watchdog
  tinyusb_device
)

target_compile_definitions(PicoUSBKeyBridge PRIVATE
  PUSBKB_GIT_COMMIT=\"${PUSBKB_GIT_COMMIT}\"
  PUSBKB_UART_INDEX=${PUSBKB_UART_INDEX}
  PUSBKB_UART_BAUDRATE=${PUSBKB_UART_BAUDRATE}
  PUSBKB_UART_TX_PIN=${PUSBKB_UART_TX_PIN}
  PUSBKB_UART_RX_PIN=${PUSBKB_UART_RX_PIN}
  $<$<BOOL:${PUSBKB_HID_TEST}>:PUSBKB_HID_TEST=1>
)

pico_enable_stdio_uart(PicoUSBKeyBridge 1)
pico_enable_stdio_usb(PicoUSBKeyBridge 0)
pico_add_extra_outputs(PicoUSBKeyBridge)
