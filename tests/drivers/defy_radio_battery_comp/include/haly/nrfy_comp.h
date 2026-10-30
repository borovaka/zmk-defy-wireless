/* SPDX-License-Identifier: MIT */

#ifndef TEST_NRFY_COMP_H
#define TEST_NRFY_COMP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t unused;
} NRF_COMP_Type;

extern NRF_COMP_Type fake_comp;
#define NRF_COMP (&fake_comp)

typedef struct {
    struct {
        uint32_t PART;
        uint32_t VARIANT;
    } INFO;
} NRF_FICR_Type;

extern NRF_FICR_Type fake_ficr;
#define NRF_FICR (&fake_ficr)
#define FICR_INFO_PART_PART_N52820 0x52820u
#define FICR_INFO_VARIANT_VARIANT_AAC0 0x41414330u
#define FICR_INFO_VARIANT_VARIANT_AAD0 0x41414430u

typedef enum {
    NRF_COMP_REF_INT_2V4 = 2,
    NRF_COMP_REF_VDD = 4,
} nrf_comp_ref_t;

enum {
    NRF_COMP_MAIN_MODE_SE,
    NRF_COMP_HYST_NO_HYST = 0,
    NRF_COMP_SP_MODE_NORMAL = 1,
    NRF_COMP_INPUT_1 = 1,
};

typedef enum {
    NRF_COMP_TASK_START,
    NRF_COMP_TASK_STOP,
} nrf_comp_task_t;

typedef enum {
    NRF_COMP_EVENT_READY,
    NRF_COMP_EVENT_DOWN,
    NRF_COMP_EVENT_UP,
    NRF_COMP_EVENT_CROSS,
} nrf_comp_event_t;

typedef struct {
    nrf_comp_ref_t reference;
    uint32_t ext_ref;
    uint32_t main_mode;
    struct {
        uint8_t th_down;
        uint8_t th_up;
    } threshold;
    uint32_t speed_mode;
    uint32_t hyst;
    uint32_t input;
} nrfy_comp_config_t;

void nrfy_comp_task_trigger(NRF_COMP_Type *reg, nrf_comp_task_t task);
void nrfy_comp_disable(NRF_COMP_Type *reg);
void nrfy_comp_enable(NRF_COMP_Type *reg);
void nrfy_comp_int_disable(NRF_COMP_Type *reg, uint32_t mask);
void nrfy_comp_shorts_disable(NRF_COMP_Type *reg, uint32_t mask);
void nrfy_comp_periph_configure(NRF_COMP_Type *reg, const nrfy_comp_config_t *config);
void nrfy_comp_event_clear(NRF_COMP_Type *reg, nrf_comp_event_t event);
bool nrfy_comp_event_check(const NRF_COMP_Type *reg, nrf_comp_event_t event);
uint32_t nrfy_comp_sample(NRF_COMP_Type *reg);

#endif
