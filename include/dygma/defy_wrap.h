/*
 * Link-time wraps of upstream functions. A module source linked with
 * -Wl,--wrap=<name> defines __wrap_<name>, which calls from other objects
 * reach in place of <name>, and reaches the original as __real_<name>. The
 * linker matches names only, so both take their type from the upstream
 * header that declares <name>: a changed signature stops the build.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DYGMA_DEFY_WRAP_H
#define DYGMA_DEFY_WRAP_H

#include <zephyr/toolchain.h>

/* Declares the original of name as __real_<name>, with name's type. */
#define DEFY_WRAP_REAL(name) __typeof__(name) __real_##name

/* After __wrap_<name> is declared: checks that it has name's type. */
#define DEFY_WRAP_CHECK(name)                                                                      \
    BUILD_ASSERT(__builtin_types_compatible_p(__typeof__(&__wrap_##name), __typeof__(&name)),      \
                 "__wrap_" #name " keeps the type of " #name)

#endif
