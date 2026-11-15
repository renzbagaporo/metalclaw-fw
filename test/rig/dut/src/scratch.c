// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <stdint.h>

#include <zephyr/toolchain.h>

// RAM the image never touches, for test/rig/probe_soak.py to write and read
// back while the image runs. Nothing references it, so scratch.ld keeps it
// from the linker's garbage collection; it is in noinit, so boot leaves it
// alone. RAM past the image is picolibc's malloc arena
// (CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE=-1), so no other RAM is free.
uint32_t mclw_dut_scratch[256] Z_GENERIC_SECTION(.mclw_dut_scratch) __aligned(4);
