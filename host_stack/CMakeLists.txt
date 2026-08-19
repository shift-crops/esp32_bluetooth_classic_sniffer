# host_stack build — included by the top-level CMakeLists via add_subdirectory(host_stack).
#
# Builds:
#   - libbtstack.so : from the PRISTINE upstream clone at ./btstack, with three files
#                     overridden by patched copies kept OUTSIDE the clone (./overlay/patched):
#                       hci.c              (LEVEL_1: report authenticated link w/o encryption)
#                       btstack_run_loop.c (idempotent timer add)
#                       hci_transport_h4.c (tolerate stray HCI command byte from the bridge)
#                     btstack_config.h is taken from ./overlay (also outside the clone).
#   - sdp_rfcomm_query / spp_counter : from ./programs + ./programs/bt_stack.cpp (port glue).
#
# All outputs go to <project>/build/host_stack/.
cmake_minimum_required(VERSION 3.10)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(BTSTACK_ROOT     "${CMAKE_CURRENT_SOURCE_DIR}/btstack")   # pristine clone (never modified)
set(OVERLAY          "${CMAKE_CURRENT_SOURCE_DIR}/overlay")   # config + patched files
set(BT_PROGRAMS_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/programs")  # our port/app sources

# generated outputs under <project>/build/host_stack (host_stack/ stays clean)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../build/host_stack")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../build/host_stack")

find_package(Threads REQUIRED)

# ---- include set (OVERLAY first so our btstack_config.h wins) ----
set(BT_INCLUDES
    ${OVERLAY}
    ${BTSTACK_ROOT}/3rd-party/micro-ecc
    ${BTSTACK_ROOT}/3rd-party/bluedroid/decoder/include
    ${BTSTACK_ROOT}/3rd-party/bluedroid/encoder/include
    ${BTSTACK_ROOT}/3rd-party/md5
    ${BTSTACK_ROOT}/3rd-party/hxcmod-player
    ${BTSTACK_ROOT}/3rd-party/hxcmod-player/mod
    ${BTSTACK_ROOT}/3rd-party/rijndael
    ${BTSTACK_ROOT}/3rd-party/yxml
    ${BTSTACK_ROOT}/3rd-party/tinydir
    ${BTSTACK_ROOT}/chipset/bcm
    ${BTSTACK_ROOT}/chipset/cc256x
    ${BTSTACK_ROOT}/chipset/csr
    ${BTSTACK_ROOT}/chipset/em9301
    ${BTSTACK_ROOT}/src
    ${BTSTACK_ROOT}/platform/posix
    ${BTSTACK_ROOT}/platform/embedded
    ${BTSTACK_ROOT})
include_directories(${BT_INCLUDES})

# ---- libbtstack.so sources (mirrors the original selection) ----
file(GLOB SOURCES_SRC       "${BTSTACK_ROOT}/src/*.c" "${BTSTACK_ROOT}/example/sco_demo_util.c")
file(GLOB SOURCES_BLE       "${BTSTACK_ROOT}/src/ble/*.c")
file(GLOB SOURCES_CLASSIC   "${BTSTACK_ROOT}/src/classic/*.c")
file(GLOB SOURCES_BLUEDROID "${BTSTACK_ROOT}/3rd-party/bluedroid/encoder/srce/*.c" "${BTSTACK_ROOT}/3rd-party/bluedroid/decoder/srce/*.c")
file(GLOB SOURCES_MD5       "${BTSTACK_ROOT}/3rd-party/md5/md5.c")
file(GLOB SOURCES_UECC      "${BTSTACK_ROOT}/3rd-party/micro-ecc/uECC.c")
file(GLOB SOURCES_YXML      "${BTSTACK_ROOT}/3rd-party/yxml/yxml.c")
file(GLOB SOURCES_HXCMOD    "${BTSTACK_ROOT}/3rd-party/hxcmod-player/*.c" "${BTSTACK_ROOT}/3rd-party/hxcmod-player/mods/*.c")
file(GLOB SOURCES_RIJNDAEL  "${BTSTACK_ROOT}/3rd-party/rijndael/rijndael.c")

set(SOURCES_POSIX
    ${BTSTACK_ROOT}/platform/posix/btstack_tlv_posix.c
    ${BTSTACK_ROOT}/platform/posix/hci_dump_posix_fs.c
    ${BTSTACK_ROOT}/platform/posix/wav_util.c
    ${BTSTACK_ROOT}/platform/posix/btstack_network_posix.c
    ${BTSTACK_ROOT}/platform/posix/btstack_uart_posix.c
    ${BTSTACK_ROOT}/platform/posix/btstack_stdin_posix.c)
set(SOURCES_CHIPSET
    ${BTSTACK_ROOT}/chipset/bcm/btstack_chipset_bcm.c
    ${BTSTACK_ROOT}/chipset/csr/btstack_chipset_csr.c)

# in-memory LE DB not used (TLV instead)
file(GLOB SOURCES_BLE_OFF "${BTSTACK_ROOT}/src/ble/le_device_db_memory.c")
list(REMOVE_ITEM SOURCES_BLE ${SOURCES_BLE_OFF})

# newer sources needing external codec libs we don't use
list(REMOVE_ITEM SOURCES_SRC
    "${BTSTACK_ROOT}/src/btstack_lc3_google.c"
    "${BTSTACK_ROOT}/src/btstack_lc3plus_fraunhofer.c")

# override the patched files (clone stays pristine)
list(REMOVE_ITEM SOURCES_SRC
    "${BTSTACK_ROOT}/src/hci.c"
    "${BTSTACK_ROOT}/src/btstack_run_loop.c"
    "${BTSTACK_ROOT}/src/hci_transport_h4.c")
set(SOURCES_OVERLAY
    ${OVERLAY}/patched/hci.c
    ${OVERLAY}/patched/btstack_run_loop.c
    ${OVERLAY}/patched/hci_transport_h4.c)

# NOTE: GATT-service profile servers and Mesh are intentionally excluded (BR/EDR sniffer;
# they pull in le-audio / src/mesh/gatt-service deps we neither need nor want to satisfy).
set(SOURCES
    ${SOURCES_MD5} ${SOURCES_YXML} ${SOURCES_BLUEDROID} ${SOURCES_POSIX} ${SOURCES_RIJNDAEL}
    ${SOURCES_SRC} ${SOURCES_BLE} ${SOURCES_CLASSIC} ${SOURCES_UECC} ${SOURCES_HXCMOD}
    ${SOURCES_CHIPSET} ${SOURCES_OVERLAY})
list(SORT SOURCES)

add_library(btstack SHARED ${SOURCES})
set_target_properties(btstack PROPERTIES PREFIX "lib" OUTPUT_NAME "btstack")

# ---- host programs (each = app.c + bt_stack.cpp + posix run loop, linked to libbtstack.so) ----
set(BT_PROGRAMS
    ${BT_PROGRAMS_DIR}/spp_counter.c
    ${BT_PROGRAMS_DIR}/sdp_rfcomm_query.c)
foreach(BT_PROGRAM_FILE ${BT_PROGRAMS})
  get_filename_component(BT_PROGRAM ${BT_PROGRAM_FILE} NAME_WE)
  add_executable(${BT_PROGRAM}
      ${BT_PROGRAM_FILE}
      ${BTSTACK_ROOT}/platform/posix/btstack_run_loop_posix.c
      ${BT_PROGRAMS_DIR}/bt_stack.cpp)
  target_link_libraries(${BT_PROGRAM} btstack Threads::Threads)
endforeach()
