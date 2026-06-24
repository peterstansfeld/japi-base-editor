# Standalone Japi Base Editor firmware = Japi Base (engine) + the editor, built
# live from the two sibling repos (no vendored copies). Mirrors the platform
# build recipe in "JapiBase/Japi Base Pico 2/CMakeLists.txt" and adds the editor
# sources, but NOT the BASIC interpreter -- this is the editor-only machine. The
# full Japi Base Computer (editor + BASIC) is built from JapiBaseBasic instead.
#
# Build (with JapiBase checked out next to this repo and PICO_SDK_PATH set):
#   cd pico && cmake -G Ninja -B build -DPICO_SDK_PATH=/path/to/pico-sdk .
#   ninja -C build            # -> build/japi_base_editor.uf2
cmake_minimum_required(VERSION 3.13)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(PICO_BOARD pico2)

include(pico_sdk_import.cmake)
project(japi_base_editor C CXX ASM)
pico_sdk_init()

# The two repos, relative to this build directory.
set(JAPIBASE "${CMAKE_CURRENT_LIST_DIR}/../../JapiBase/Japi Base Pico 2")
set(EDITOR   "${CMAKE_CURRENT_LIST_DIR}/../editor")
set(CALC     "${CMAKE_CURRENT_LIST_DIR}/../JapiBaseCalc/src")

add_executable(japi_base_editor
    main.c

    # --- Japi Base platform/engine (canonical copy in JapiBase) ---
    "${JAPIBASE}/japi_base.c"
    "${JAPIBASE}/japi_pin_config.c"
    "${JAPIBASE}/third_party_libs.c"

    # --- Editor (this repo) ---
    "${EDITOR}/jbe.c"
    "${EDITOR}/ui_filelist.c"
    "${EDITOR}/jbe_main_pico.c"

    # --- Programmer's + classic calculator (JapiBaseCalc submodule, NumLock) ---
    "${CALC}/calc.c"
    "${CALC}/calc_engine.c"
    "${CALC}/calc_ui.c"
    "${CALC}/calc_eval.c"
    "${CALC}/calc_classic.c"
)

pico_generate_pio_header(japi_base_editor "${JAPIBASE}/japi_base.pio")

pico_enable_stdio_uart(japi_base_editor 1)
pico_enable_stdio_usb(japi_base_editor 0)

target_compile_definitions(japi_base_editor PRIVATE
    JAPI_PICO                  # select the Pico entry + Pico-side conditionals
    LFS_NO_DEBUG=1             # littlefs is used only from Core 0 -> no LFS_THREADSAFE
)
# Note: JBB_RUN is intentionally NOT defined -- no BASIC interpreter here, so the
# editor's Run menu offers only the Japi Commander.

target_include_directories(japi_base_editor PRIVATE
    "${JAPIBASE}"
    "${EDITOR}"
    "${CALC}"
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(japi_base_editor
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_vreg
    hardware_watchdog
    hardware_irq
    hardware_clocks
    hardware_pwm
    hardware_spi
    hardware_flash
    pico_multicore
)

pico_add_extra_outputs(japi_base_editor)
