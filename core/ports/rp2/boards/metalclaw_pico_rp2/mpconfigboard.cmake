# MetalClaw Pico RP2 v1.r1 (RP2354B carrier for a Pico or Pico 2 DUT) as
# the Metalclaw harness. pico-sdk has no header for it; it is in this dir.
list(APPEND PICO_BOARD_HEADER_DIRS ${MICROPY_BOARD_DIR})
set(PICO_BOARD "metalclaw_pico_rp2")
set(PICO_NUM_GPIOS 48)
