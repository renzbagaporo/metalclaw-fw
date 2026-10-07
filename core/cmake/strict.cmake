# SPDX-License-Identifier: Apache-2.0 OR MIT
# No-warnings policy. Metalclaw sources get METALCLAW_STRICT_C_OPTIONS per
# source file. Third-party code gets METALCLAW_THIRD_PARTY_C_OPTIONS through
# the firmware target: the same set minus what MicroPython, pico-sdk and
# TinyUSB cannot meet (measured on v1.28.0 / 2.2.0: -Wmissing-prototypes,
# -Wshadow, -Wnull-dereference, -Wstrict-prototypes, -Wvla,
# and -Wextra's implicit-fallthrough, sign-compare,
# missing-field-initializers, type-limits, shift-negative-value and
# old-style-declaration). -Wundef, -Wredundant-decls and -Wunused-parameter
# are in neither set because they fire inside MicroPython's headers, which
# our sources include.

set(METALCLAW_THIRD_PARTY_C_OPTIONS
    -Wextra
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-missing-field-initializers
    -Wno-type-limits
    -Wno-shift-negative-value
    -Wno-old-style-declaration
    -Wno-implicit-fallthrough
    -Wpointer-arith
    -Wdouble-promotion
    -Wformat=2
    -Werror
)

set(METALCLAW_STRICT_C_OPTIONS
    ${METALCLAW_THIRD_PARTY_C_OPTIONS}
    -Wsign-compare
    -Wmissing-field-initializers
    -Wtype-limits
    -Wshift-negative-value
    -Wold-style-declaration
    -Wimplicit-fallthrough
    -Wshadow
    -Wstrict-prototypes
    -Wmissing-prototypes
    -Wvla
    -Wnull-dereference
)

function(metalclaw_strict_sources)
    set_source_files_properties(${ARGV} PROPERTIES COMPILE_OPTIONS "${METALCLAW_STRICT_C_OPTIONS}")
endfunction()

function(metalclaw_strict_third_party target)
    target_compile_options(${target} INTERFACE "$<$<COMPILE_LANGUAGE:C>:${METALCLAW_THIRD_PARTY_C_OPTIONS}>")
endfunction()
