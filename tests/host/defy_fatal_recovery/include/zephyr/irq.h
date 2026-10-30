/* SPDX-License-Identifier: MIT */

#pragma once
unsigned int irq_lock(void);

void irq_unlock(unsigned int key);
