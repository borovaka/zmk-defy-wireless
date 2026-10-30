/* SPDX-License-Identifier: MIT */

#pragma once
struct k_work { int unused; };
struct k_work_delayable { void (*handler)(struct k_work *); };
#define K_WORK_DELAYABLE_DEFINE(name, fn) struct k_work_delayable name = {fn}
#define K_SECONDS(s) ((s) * 1000)
int k_work_schedule(struct k_work_delayable *work, int delay);
