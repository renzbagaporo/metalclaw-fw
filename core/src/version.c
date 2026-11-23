// SPDX-License-Identifier: Apache-2.0 OR MIT
// The build's version string, which core/cmake/build_version.cmake writes into
// build_version.h at every build.

#include "metalclaw/version.h"

#include "build_version.h"

_Static_assert(MCLW_VERSION_MAJOR < 100 && MCLW_VERSION_MINOR < 10 && MCLW_VERSION_PATCH < 10,
    "the version must fit a USB bcdDevice");

const char *mclw_version_string(void) {
    return MCLW_BUILD_VERSION;
}
