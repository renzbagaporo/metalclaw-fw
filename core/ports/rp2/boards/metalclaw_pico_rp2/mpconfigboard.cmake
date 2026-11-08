# MetalClaw Pico RP2 v1.r1 (RP2354B carrier for a Pico or Pico 2 DUT) as
# the Metalclaw harness. pico-sdk has no header for it; it is in this dir.
list(APPEND PICO_BOARD_HEADER_DIRS ${MICROPY_BOARD_DIR})
set(PICO_BOARD "metalclaw_pico_rp2")
set(PICO_NUM_GPIOS 48)

# v1.r1 pulls every Pico GPIO net up to the Pico's 3V3_OUT through 5.1 kOhm.
# Turn it off to build for a revision without the pull-ups; the harness
# reports the setting in metalclaw.board().
option(METALCLAW_LINE_PULLUPS "the board pulls every DUT line up" ON)
if(METALCLAW_LINE_PULLUPS)
    list(APPEND MICROPY_DEF_BOARD METALCLAW_LINE_PULLUPS=1)
endif()
