// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef MCLW_VERSION_H
#define MCLW_VERSION_H

#define MCLW_VERSION_STRING "0.0.0"

// Bumped whenever a struct or enum in metalclaw/*.h changes shape or meaning.
// A model declares the version it was written against and the build refuses
// a mismatch.
#define MCLW_ABI_VERSION 1

#endif
