/*
 * A clock the test sets in now. A suite includes this header after its other
 * headers; from there to its #undef k_uptime_get, which follows the module
 * sources it includes, k_uptime_get() returns now.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DEFY_TEST_FAKE_UPTIME_H
#define DEFY_TEST_FAKE_UPTIME_H

#include <stdint.h>

#include <zephyr/kernel.h>

static int64_t now;
static int64_t test_uptime(void) { return now; }
#define k_uptime_get test_uptime

#endif
