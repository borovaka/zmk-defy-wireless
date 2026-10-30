/*
 * I2C diagnostics for the battery samples. Linked with
 * --wrap=nrfx_twim_init, it puts itself between the TWIM1 driver and its
 * event handler, to count the bus events of each read and keep the last one.
 * When a gauge read fails, the sample becomes an I2C diagnostic sample: the
 * register, the error and how long it took, the events, and the bus pins'
 * levels and configuration. After an address NACK, battery.c probes the
 * charger as well, and its result joins the sample.
 *
 * SPDX-License-Identifier: MIT
 */

#include <nrfx_twim.h>
#include <hal/nrf_gpio.h>
#include <zephyr/kernel.h>

#include <dygma/defy_wrap.h>

#include "battery_trace.h"

static struct k_spinlock trace_lock;
static nrfx_twim_evt_handler_t original_handler;
static uint8_t trace_count;
static uint16_t trace_event;

DEFY_WRAP_REAL(nrfx_twim_init);

static void trace_handler(nrfx_twim_evt_t const *event, void *context) {
    k_spinlock_key_t key = k_spin_lock(&trace_lock);
    trace_count = MIN((unsigned int)trace_count + 1, UINT8_MAX);
    trace_event = (uint8_t)event->type | (uint16_t)(uint8_t)event->xfer_desc.type << 8;
    nrfx_twim_evt_handler_t handler = original_handler;
    k_spin_unlock(&trace_lock, key);
    handler(event, context);
}

nrfx_err_t __wrap_nrfx_twim_init(nrfx_twim_t const *instance, nrfx_twim_config_t const *config,
                                nrfx_twim_evt_handler_t handler, void *context) {
    if (instance->p_twim != NRF_TWIM1 || handler == NULL) {
        return __real_nrfx_twim_init(instance, config, handler, context);
    }
    nrfx_err_t result = __real_nrfx_twim_init(instance, config, trace_handler, context);
    if (result == NRFX_SUCCESS) {
        k_spinlock_key_t key = k_spin_lock(&trace_lock);
        original_handler = handler;
        k_spin_unlock(&trace_lock, key);
    }
    return result;
}
DEFY_WRAP_CHECK(nrfx_twim_init);

void defy_battery_trace_begin(void) {
    k_spinlock_key_t key = k_spin_lock(&trace_lock);
    trace_count = 0;
    trace_event = DEFY_BATTERY_I2C_NO_EVENT;
    k_spin_unlock(&trace_lock, key);
}

void defy_battery_trace_error(struct defy_battery_sample *sample, uint8_t reg, int error,
                              uint32_t elapsed_us) {
    k_spinlock_key_t key = k_spin_lock(&trace_lock);
    uint8_t count = trace_count;
    uint16_t event = trace_event;
    uint32_t sda = NRF_TWIM1->PSEL.SDA;
    uint32_t scl = NRF_TWIM1->PSEL.SCL;
    uint32_t inputs = NRF_P0->IN;
    uint16_t pins = (sda & 31u) | (scl & 31u) << 5;
    uint16_t configuration = 0;
    if (sda & BIT(31)) {
        pins |= DEFY_BATTERY_I2C_SDA_DISCONNECTED;
    } else {
        pins |= inputs & BIT(sda & 31u) ? DEFY_BATTERY_I2C_SDA_HIGH : 0;
        configuration |= NRF_P0->PIN_CNF[sda & 31u] & 0xffu;
    }
    if (scl & BIT(31)) {
        pins |= DEFY_BATTERY_I2C_SCL_DISCONNECTED;
    } else {
        pins |= inputs & BIT(scl & 31u) ? DEFY_BATTERY_I2C_SCL_HIGH : 0;
        configuration |= (NRF_P0->PIN_CNF[scl & 31u] & 0xffu) << 8;
    }
    pins |= NRF_TWIM1->ENABLE ? DEFY_BATTERY_I2C_ENABLED : 0;
    k_spin_unlock(&trace_lock, key);
    sample->status = DEFY_BATTERY_SAMPLE_I2C_DIAGNOSTIC;
    sample->words[DEFY_BATTERY_I2C_TAG] = DEFY_BATTERY_I2C_DIAGNOSTIC_TAG;
    sample->words[DEFY_BATTERY_I2C_REGISTER_EVENTS] = reg | (uint16_t)count << 8;
    sample->words[DEFY_BATTERY_I2C_ERRNO] = (uint16_t)error;
    sample->words[DEFY_BATTERY_I2C_ELAPSED_LOW] = elapsed_us;
    sample->words[DEFY_BATTERY_I2C_ELAPSED_HIGH] = elapsed_us >> 16;
    sample->words[DEFY_BATTERY_I2C_EVENT_TYPE] = event;
    sample->words[DEFY_BATTERY_I2C_PINS] = pins;
    sample->words[DEFY_BATTERY_I2C_PIN_CNF_LOW8] = configuration;
}

bool defy_battery_trace_tx_address_nack(const struct defy_battery_sample *sample) {
    return sample->status == DEFY_BATTERY_SAMPLE_I2C_DIAGNOSTIC &&
           sample->words[DEFY_BATTERY_I2C_TAG] == DEFY_BATTERY_I2C_DIAGNOSTIC_TAG &&
           sample->words[DEFY_BATTERY_I2C_REGISTER_EVENTS] >> 8 &&
           sample->words[DEFY_BATTERY_I2C_EVENT_TYPE] ==
               (NRFX_TWIM_EVT_ADDRESS_NACK | NRFX_TWIM_XFER_TX << 8);
}

static uint16_t pack_trace(uint8_t count, uint16_t event) {
    if (event == DEFY_BATTERY_I2C_NO_EVENT) {
        return (uint16_t)count << 8 | DEFY_BATTERY_I2C_PACKED_NO_EVENT << 4 |
               DEFY_BATTERY_I2C_PACKED_NO_EVENT;
    }
    uint8_t type = MIN(event >> 8, DEFY_BATTERY_I2C_PACKED_UNKNOWN);
    uint8_t kind = MIN(event & 0xff, DEFY_BATTERY_I2C_PACKED_UNKNOWN);
    return (uint16_t)count << 8 | type << 4 | kind;
}

static uint8_t elapsed_ms(uint32_t elapsed_us) {
    return MIN(elapsed_us / 1000u + (elapsed_us % 1000u != 0), UINT8_MAX);
}

void defy_battery_trace_charger(struct defy_battery_sample *sample, uint8_t reg, int error,
                               uint8_t value, uint32_t elapsed_us) {
    k_spinlock_key_t key = k_spin_lock(&trace_lock);
    uint16_t charger_trace = pack_trace(trace_count, trace_event);
    k_spin_unlock(&trace_lock, key);
    uint16_t gauge_register_events = sample->words[DEFY_BATTERY_I2C_REGISTER_EVENTS];
    uint16_t gauge_errno = sample->words[DEFY_BATTERY_I2C_ERRNO];
    uint16_t gauge_trace = pack_trace(gauge_register_events >> 8,
                                     sample->words[DEFY_BATTERY_I2C_EVENT_TYPE]);
    uint32_t gauge_us = sample->words[DEFY_BATTERY_I2C_ELAPSED_LOW] |
                        (uint32_t)sample->words[DEFY_BATTERY_I2C_ELAPSED_HIGH] << 16;
    sample->words[DEFY_BATTERY_I2C_CHARGER_FORMAT] = DEFY_BATTERY_I2C_CHARGER_TAG;
    sample->words[DEFY_BATTERY_I2C_PROBE_REGISTERS] =
        (gauge_register_events & 0xff) | (uint16_t)reg << 8;
    sample->words[DEFY_BATTERY_I2C_GAUGE_ERRNO] = gauge_errno;
    sample->words[DEFY_BATTERY_I2C_GAUGE_TRACE] = gauge_trace;
    sample->words[DEFY_BATTERY_I2C_CHARGER_ERRNO] = error;
    sample->words[DEFY_BATTERY_I2C_CHARGER_VALUE] = error ? DEFY_BATTERY_I2C_NO_VALUE : value;
    sample->words[DEFY_BATTERY_I2C_CHARGER_TRACE] = charger_trace;
    sample->words[DEFY_BATTERY_I2C_PROBE_ELAPSED_MS] =
        elapsed_ms(gauge_us) | (uint16_t)elapsed_ms(elapsed_us) << 8;
}
