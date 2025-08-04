# CMakeLists
#
# MIT License
#
# Copyright (c) 2021, 2024 Matt Evans
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#
#
cmake_minimum_required(VERSION 3.13)

# Options that should be defined when initialising the build
# directory with cmake, e.g. "cmake .. -DOPTION=true":
#
# Note: to build for pico2 / rp2350: cmake .. -DPICO_BOARD=pico2
# Note: to build for fruit jam:
#    cmake -DBOARD=adafruit_fruit_jam -DPICO_BOARD=pico2 -DUSE_HSTX=1 -S . -B build_hstx -DPICO_SDK_PATH=../pico-sdk -DSD_TX=35 -DSD_RX=36 -DSD_SCK=34 -DSD_CS=39

set(PICO_TINYUSB_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lib/tinyusb)
set(PIOUSB_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lib/Pico-PIO-USB)

set(UART_TX CACHE STRING "")
set(UART_RX CACHE STRING "")
set(UART CACHE STRING "")

option(USE_SD "Build in SD support" OFF)
set(SD_TX 3 CACHE STRING "SD SPI TX pin")
set(SD_RX 4 CACHE STRING "SD SPI RX pin")
set(SD_SCK 2 CACHE STRING "SD SPI SCK pin")
set(SD_CS 5 CACHE STRING "SD SPI CS pin")
set(SD_MHZ 5 CACHE STRING "SD SPI speed in MHz")

option(USE_HSTX "Use HSTX digital video (only for rp2350 / pico2)" OFF)

# Options for HSTX output (defaults are for Adafruit FruitJam)
# HSTX always uses 640x480 for now
set(HSTX_CKP 13 CACHE STRING "HSTX CK+ PIN")
set(HSTX_D0P 15 CACHE STRING "HSTX D0+ PIN")
set(HSTX_D1P 17 CACHE STRING "HSTX D1+ PIN")
set(HSTX_D2P 19 CACHE STRING "HSTX D2+ PIN")

option(OVERCLOCK "Overclock to 264MHz (known incompatible with psram)" OFF)

# Options for analog VGA output
option(USE_VGA_RES "Video uses VGA (640x480) resolution" OFF)
set(VIDEO_PIN 18 CACHE STRING "VGA Video GPIO base pin (followed by VS, CLK, HS)")

option(USE_PSRAM "Locate main Mac ram in PSRAM (only for rp2350 / pico 2)" OFF)
set(PSRAM_CS 47 CACHE STRING "PSRAM Chip select pin")

# Pins for PIO-based USB host
set(PIN_USB_HOST_DP 1 CACHE STRING "USB D+ PIN")
set(PIN_USB_HOST_DM 2 CACHE STRING "USB D- PIN")

set(USE_AUDIO 1 CACHE STRING "Use audio")
set(PIN_AUDIO_PWM 41 CACHE STRING "Pin for PWM audio")

# See below, -DMEMSIZE=<size in KB> will configure umac's memory size,
# overriding defaults.
set(MEMSIZE 128 CACHE STRING "Memory size, in KB")

set(DISC_IMAGE ${CMAKE_CURRENT_SOURCE_DIR}/umac0ro.img CACHE FILEPATH "Built-in disk image")

if (USE_HSTX)
   add_compile_definitions(USE_VGA_RES=1)
   add_compile_definitions(HSTX_CKP=${HSTX_CKP} HSTX_D0P=${HSTX_D0P} HSTX_D1P=${HSTX_D1P} HSTX_D2P=${HSTX_D2P})
   set(VIDEO_SRC src/video_hstx.c)
else()
  add_compile_definitions(GPIO_VID_BASE=${VIDEO_PIN})
  set(VIDEO_SRC src/video_vga.c)
endif()

if (OVERCLOCK)
  add_compile_definitions(OVERCLOCK=1)
  set(OPT_OC "-oc")
else()
  set(OPT_OC "")
endif()

if (USE_VGA_RES)
 add_compile_definitions(USE_VGA_RES=1)
 add_compile_definitions(DISP_WIDTH=640)
 add_compile_definitions(DISP_HEIGHT=480)
 set(RES "640x480")
 set(RESFLAG "-v")
else()
 add_compile_definitions(DISP_WIDTH=512)
 add_compile_definitions(DISP_HEIGHT=342)
 set(RES "512x342")
 set(RESFLAG "")
endif()

if (USE_PSRAM)
  add_compile_definitions(PIN_PSRAM_CS=${PSRAM_CS} USE_PSRAM=1)
  set(OPT_PSRAM "-psram")
else()
  add_compile_definitions(USE_PSRAM=0)
  set(OPT_PSRAM "")
endif()

set(FIRMWARE "pico-mac-${PICO_BOARD}-${MEMSIZE}k-${RES}${OPT_PSRAM}${OPT_OC}")


# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)

project(${FIRMWARE})

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# For TUSB host stuff:
set(FAMILY rp2040)
set(BOARD raspberry_pi_pico)

# umac subproject (and Musashi sub-subproject)
set(UMAC_PATH ${CMAKE_CURRENT_SOURCE_DIR}/external/umac)
set(UMAC_MUSASHI_PATH ${UMAC_PATH}/external/Musashi)
set(UMAC_INCLUDE_PATHS ${UMAC_PATH}/include ${UMAC_MUSASHI_PATH})

# This isn't very nice, but hey it's Sunday :p
set(UMAC_SOURCES
  ${UMAC_PATH}/src/disc.c
  ${UMAC_PATH}/src/main.c
  ${UMAC_PATH}/src/rom.c
  ${UMAC_PATH}/src/scc.c
  ${UMAC_PATH}/src/via.c
  ${UMAC_MUSASHI_PATH}/m68kcpu.c
  ${UMAC_MUSASHI_PATH}/m68kdasm.c
  ${UMAC_MUSASHI_PATH}/m68kops.c
  ${UMAC_MUSASHI_PATH}/softfloat/softfloat.c
  )

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ggdb -g3 -O3 -DPICO -DMUSASHI_CNF=\\\"../include/m68kconf.h\\\" -DUMAC_MEMSIZE=${MEMSIZE}")


if (USE_SD)
   add_compile_definitions(USE_SD=1)
   set(FF_DISABLE_RTC ${PICO_RP2350})  # RP2350 doesn't have RTC, so disable it
   add_subdirectory(external/no-OS-FatFS-SD-SPI-RPi-Pico/FatFs_SPI build)
   set(EXTRA_SD_SRC src/sd_hw_config.c)
   set(EXTRA_SD_LIB FatFs_SPI)
   add_compile_definitions(SD_TX=${SD_TX} SD_RX=${SD_RX} SD_SCK=${SD_SCK} SD_CS=${SD_CS} SD_MHZ=${SD_MHZ})
endif()

add_compile_definitions(PIN_USB_HOST_DP=${PIN_USB_HOST_DP})
add_compile_definitions(PIN_USB_HOST_DM=${PIN_USB_HOST_DM})
add_compile_definitions(PICO_DEFAULT_PIO_USB_DP_PIN=${PIN_USB_HOST_DP})
add_compile_definitions(PICO_DEFAULT_PIO_USB_DM_PIN=${PIN_USB_HOST_DM})

if (NOT UART STREQUAL "")
  add_compile_definitions(PICO_DEFAULT_UART=${UART})
endif()
if (NOT UART_TX STREQUAL "")
  add_compile_definitions(PICO_DEFAULT_UART_TX_PIN=${UART_TX})
endif()
if (NOT UART_RX STREQUAL "")
  add_compile_definitions(PICO_DEFAULT_UART_RX_PIN=${UART_RX})
endif()

if (USE_AUDIO)
   add_subdirectory(external/pico-extras/src/rp2_common/pico_audio_i2s)
   add_subdirectory(external/pico-extras/src/common/pico_audio)
   add_subdirectory(external/pico-extras/src/common/pico_util_buffer)
   add_compile_definitions(ENABLE_AUDIO=1 PICO_AUDIO_I2S_PIO=1 PICO_AUDIO_I2S_DMA_IRQ=0 PICO_AUDIO_I2S_DATA_PIN=24 PICO_AUDIO_I2S_CLOCK_PIN_BASE=26 PICO_AUDIO_I2S_MONO_INPUT=1)
   set(EXTRA_AUDIO_LIB pico_util_buffer pico_audio pico_audio_i2s hardware_i2c)
endif()

if (TARGET tinyusb_device)
  add_executable(${FIRMWARE}
    src/main.c
    ${VIDEO_SRC}
    src/kbd.c
    src/hid.c
    src/clocking.c
    ${EXTRA_SD_SRC}

    ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
    ${PIOUSB_PATH}/src/pio_usb.c
    ${PIOUSB_PATH}/src/pio_usb_host.c
    ${PIOUSB_PATH}/src/usb_crc.c

    ${UMAC_SOURCES}
    )

  # The umac sources need to prepare Musashi (some sources are generated):
  add_custom_command(OUTPUT incbin/umac-rom.h
    COMMAND echo "*** Patching ROM ***"
    COMMAND set -xe && mkdir -p ${CMAKE_CURRENT_BINARY_DIR}/incbin && make -C ${UMAC_PATH} patcher && ${UMAC_PATH}/patcher ${RESFLAG} -m ${MEMSIZE} -r "${CMAKE_CURRENT_LIST_DIR}/rom.bin" -w ${CMAKE_CURRENT_BINARY_DIR}/incbin/umac-rom.h
    )
  add_custom_target(prepare_rom
    DEPENDS incbin/umac-rom.h
    )

  add_custom_command(OUTPUT incbin/umac-disc.h
    COMMAND echo "DISC_IMAGE is ${DISC_IMAGE}" && mkdir -p ${CMAKE_CURRENT_BINARY_DIR}/incbin && xxd -i < "${DISC_IMAGE}" > ${CMAKE_CURRENT_BINARY_DIR}/incbin/umac-disc.h
    )
  add_custom_target(prepare_disc
    DEPENDS incbin/umac-disc.h
    )

  add_custom_command(OUTPUT ${UMAC_MUSASHI_PATH}/m68kops.c
    COMMAND echo "*** Preparing umac source ***"
    COMMAND make -C ${UMAC_PATH} prepare
    )
  add_custom_target(prepare_umac
    DEPENDS ${UMAC_MUSASHI_PATH}/m68kops.c
    )
  add_dependencies(${FIRMWARE} prepare_umac prepare_rom prepare_disc)

  target_link_libraries(${FIRMWARE}
    pico_stdlib
    pico_multicore
    tinyusb_host
    tinyusb_board
    hardware_dma
    hardware_pio
    hardware_sync
    ${EXTRA_SD_LIB}
    ${EXTRA_AUDIO_LIB}
    )

  target_include_directories(${FIRMWARE} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${PICO_TINYUSB_PATH}/hw
    ${PICO_TINYUSB_PATH}/src
    ${UMAC_INCLUDE_PATHS}
    ${PIOUSB_PATH}/src
    ${CMAKE_CURRENT_BINARY_DIR}/incbin
    ${CMAKE_CURRENT_LIST_DIR}
    )

  if (NOT USE_HSTX)
    pico_generate_pio_header(${FIRMWARE} ${CMAKE_CURRENT_LIST_DIR}/src/pio_video.pio)
  endif()

  pico_enable_stdio_uart(${FIRMWARE} 1)

  # Needed for UF2:
  pico_add_extra_outputs(${FIRMWARE})

elseif(PICO_ON_DEVICE)
   message(WARNING "not building firmware because TinyUSB submodule is not initialized in the SDK")
endif()

