// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef MCLW_VERSION_H
#define MCLW_VERSION_H

// The release this tree builds: a release tag is v<major>.<minor>.<patch> of
// these numbers (core/cmake/build_version.cmake checks the tag against them).
#define MCLW_VERSION_MAJOR 0
#define MCLW_VERSION_MINOR 1
#define MCLW_VERSION_PATCH 0

// As a USB bcdDevice, 0xJJMN: the major version in two BCD digits, then the
// minor and the patch in one each.
#define MCLW_VERSION_BCD                                                                         \
    ((((MCLW_VERSION_MAJOR / 10) % 10) << 12) | ((MCLW_VERSION_MAJOR % 10) << 8) |               \
        ((MCLW_VERSION_MINOR % 10) << 4) | (MCLW_VERSION_PATCH % 10))

// The build's version string: "0.1.0" for a build of the release's tagged
// commit, "0.1.0-3-gabc1234def0" for a later commit, "-dirty" after it for
// a tree with uncommitted changes, and "0.1.0" alone for a tree outside git.
const char *mclw_version_string(void);

// Bumped whenever a struct or enum in metalclaw/*.h changes shape or meaning.
// A model declares the version it was written against and the build refuses
// a mismatch.
#define MCLW_ABI_VERSION 1

#endif
