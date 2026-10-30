/*
 * The battery voltage from the nRF52820's comparator, on a half whose gauge
 * does not answer. The battery, divided down, is on AIN1. A sweep tries the
 * 64 thresholds from the top against the internal 2.4 V reference, and
 * against VDD if the input is above all of them; the first threshold the
 * input is above is the reading. The registers are driven through nrfy,
 * with Zephyr's comparator drivers off. Stopping the comparator applies
 * erratum 275's workaround on the nRF52820 builds it affects.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <haly/nrfy_comp.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

#include "battery_comp.h"

#define COMP_READY_TIMEOUT_US 1000u
/* TH is six bits. */
#define COMP_THRESHOLD_MAX 63
#define COMP_ERRATA_275_RESET 0x40013ffcu
#define FICR_VARIANT_BUILD_MASK 0x0000ff00u

BUILD_ASSERT(!IS_ENABLED(CONFIG_NRFX_COMP));
BUILD_ASSERT(!IS_ENABLED(CONFIG_COMPARATOR));

static bool comp_errata_275(void) {
    uint32_t build = NRF_FICR->INFO.VARIANT & FICR_VARIANT_BUILD_MASK;
    return NRF_FICR->INFO.PART == FICR_INFO_PART_PART_N52820 &&
           (build == (FICR_INFO_VARIANT_VARIANT_AAC0 & FICR_VARIANT_BUILD_MASK) ||
            build == (FICR_INFO_VARIANT_VARIANT_AAD0 & FICR_VARIANT_BUILD_MASK));
}

static void comp_stop(void) {
    nrfy_comp_task_trigger(NRF_COMP, NRF_COMP_TASK_STOP);
    nrfy_comp_disable(NRF_COMP);
    if (comp_errata_275()) {
        sys_write32(0, COMP_ERRATA_275_RESET);
        (void)sys_read32(COMP_ERRATA_275_RESET);
        sys_write32(1, COMP_ERRATA_275_RESET);
    }
}

static int comp_trial(uint8_t threshold, nrf_comp_ref_t reference) {
    nrfy_comp_config_t config = {
        .reference = reference,
        .main_mode = NRF_COMP_MAIN_MODE_SE,
        .threshold = {.th_down = threshold, .th_up = threshold},
        .speed_mode = NRF_COMP_SP_MODE_NORMAL,
        .hyst = NRF_COMP_HYST_NO_HYST,
        .input = NRF_COMP_INPUT_1,
    };

    nrfy_comp_periph_configure(NRF_COMP, &config);
    nrfy_comp_enable(NRF_COMP);
    nrfy_comp_task_trigger(NRF_COMP, NRF_COMP_TASK_STOP);
    nrfy_comp_event_clear(NRF_COMP, NRF_COMP_EVENT_READY);
    nrfy_comp_event_clear(NRF_COMP, NRF_COMP_EVENT_DOWN);
    nrfy_comp_event_clear(NRF_COMP, NRF_COMP_EVENT_UP);
    nrfy_comp_event_clear(NRF_COMP, NRF_COMP_EVENT_CROSS);
    uint32_t started = k_cycle_get_32();
    nrfy_comp_task_trigger(NRF_COMP, NRF_COMP_TASK_START);

    int result = -ETIMEDOUT;
    while (!nrfy_comp_event_check(NRF_COMP, NRF_COMP_EVENT_READY)) {
        if (k_cyc_to_us_floor64((uint32_t)(k_cycle_get_32() - started)) >=
            COMP_READY_TIMEOUT_US) {
            goto done;
        }
        k_busy_wait(1);
    }
    result = nrfy_comp_sample(NRF_COMP);

done:
    comp_stop();
    return result;
}

static int comp_sweep(nrf_comp_ref_t reference, uint8_t *threshold) {
    for (int value = COMP_THRESHOLD_MAX;; value--) {
        int above = comp_trial(value, reference);

        if (above < 0) {
            return above;
        }
        if (above || value == 0) {
            *threshold = value;
            return 0;
        }
    }
}

int defy_radio_battery_comp_read(struct defy_battery_comp_reading *out) {
    nrfy_comp_int_disable(NRF_COMP, UINT32_MAX);
    nrfy_comp_shorts_disable(NRF_COMP, UINT32_MAX);
    comp_stop();
    struct defy_battery_comp_reading reading = {
        .reference = DEFY_BATTERY_ANALOG_REFERENCE_2V4,
    };
    int err = comp_sweep(NRF_COMP_REF_INT_2V4, &reading.threshold);
    if (err) {
        return err;
    }
    if (reading.threshold == COMP_THRESHOLD_MAX) {
        reading.reference = DEFY_BATTERY_ANALOG_REFERENCE_VDD;
        err = comp_sweep(NRF_COMP_REF_VDD, &reading.threshold);
        if (err) {
            return err;
        }
    }
    *out = reading;
    return 0;
}
