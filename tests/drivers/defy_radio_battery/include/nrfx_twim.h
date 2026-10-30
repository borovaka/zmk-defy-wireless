/* SPDX-License-Identifier: MIT */

#ifndef DEFY_TEST_NRFX_TWIM_H
#define DEFY_TEST_NRFX_TWIM_H

#include <stdint.h>

typedef uint32_t nrfx_err_t;
#define NRFX_SUCCESS 0u

typedef struct {
    struct { uint32_t SCL; uint32_t SDA; } PSEL;
    uint32_t ENABLE;
} NRF_TWIM_Type;
extern NRF_TWIM_Type fake_twim1;
#define NRF_TWIM1 (&fake_twim1)

typedef struct { NRF_TWIM_Type *p_twim; uint8_t drv_inst_idx; } nrfx_twim_t;
typedef struct { uint32_t frequency; } nrfx_twim_config_t;
typedef enum {
    NRFX_TWIM_EVT_DONE,
    NRFX_TWIM_EVT_ADDRESS_NACK,
    NRFX_TWIM_EVT_DATA_NACK,
    NRFX_TWIM_EVT_OVERRUN,
    NRFX_TWIM_EVT_BUS_ERROR,
} nrfx_twim_evt_type_t;
typedef enum {
    NRFX_TWIM_XFER_TX,
    NRFX_TWIM_XFER_RX,
    NRFX_TWIM_XFER_TXRX,
    NRFX_TWIM_XFER_TXTX,
} nrfx_twim_xfer_type_t;
typedef struct { nrfx_twim_xfer_type_t type; } nrfx_twim_xfer_desc_t;
typedef struct {
    nrfx_twim_evt_type_t type;
    nrfx_twim_xfer_desc_t xfer_desc;
} nrfx_twim_evt_t;
typedef void (*nrfx_twim_evt_handler_t)(nrfx_twim_evt_t const *, void *);

/* As nrfx declares it, for battery_trace.c's wrap. */
nrfx_err_t nrfx_twim_init(nrfx_twim_t const *p_instance, nrfx_twim_config_t const *p_config,
                          nrfx_twim_evt_handler_t event_handler, void *p_context);

#endif
