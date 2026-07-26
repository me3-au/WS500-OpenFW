/*
 * version.h — firmware version identity.
 *
 * Single source of truth for the code; keep in sync with the top-level VERSION
 * file and CHANGELOG.md. Bump per semver; the "-dev" suffix is dropped only on
 * a tagged release (first tag: v0.1.0 at milestone M6 — see docs/PROJECT_PLAN.md).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_VERSION_H
#define WS500_VERSION_H

#include <stdint.h>

#define FW_VERSION_MAJOR   0
#define FW_VERSION_MINOR   1
#define FW_VERSION_PATCH   0

/* Human-readable form — telemetry, USB CDC banner, CAN product info. */
#define FW_VERSION_STRING  "0.1.0-dev"

/*
 * Short git hash of the build, injected by the build system as
 * -DFW_GIT_HASH="\"abc1234\"". Empty when nothing supplied one, and an empty
 * value makes the hello handshake OMIT the "git" field (docs/VERSIONING.md §2)
 * rather than report a placeholder identity — an unidentifiable build must look
 * unidentifiable, not look like a release.
 */
#ifndef FW_GIT_HASH
#define FW_GIT_HASH        ""
#endif

/* Packed BCD form 0x00MMmmpp (major/minor/patch, one byte each) for compact
 * reporting, e.g. 0x00000100 = 0.1.0. Dev/pre-release status is not encoded. */
#define FW_VERSION_BCD                                             \
    (((uint32_t)(((FW_VERSION_MAJOR / 10) << 4) | (FW_VERSION_MAJOR % 10)) << 16) | \
     ((uint32_t)(((FW_VERSION_MINOR / 10) << 4) | (FW_VERSION_MINOR % 10)) << 8)  | \
     ((uint32_t)(((FW_VERSION_PATCH / 10) << 4) | (FW_VERSION_PATCH % 10))))

#endif /* WS500_VERSION_H */
