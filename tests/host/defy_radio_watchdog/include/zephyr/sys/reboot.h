/* SPDX-License-Identifier: MIT */

#pragma once

#define SYS_REBOOT_COLD 1

_Noreturn void sys_reboot(int type);
