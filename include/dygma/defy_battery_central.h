/* SPDX-License-Identifier: MIT */

#ifndef DYGMA_DEFY_BATTERY_CENTRAL_H
#define DYGMA_DEFY_BATTERY_CENTRAL_H

#include <dygma/defy_battery.h>
#include <dygma/defy_control.h>

#define DEFY_BATTERY_DIAGNOSTIC_SIZE (4u + DEFY_BATTERY_FRAME_SIZE)
/* The USB snapshot: an 8-byte header, then one 48-byte record per side. */
#define DEFY_BATTERY_SNAPSHOT_VERSION 1u
#define DEFY_BATTERY_SNAPSHOT_HEADER 8u
#define DEFY_BATTERY_SNAPSHOT_RECORD_SIZE 48u
#define DEFY_BATTERY_SNAPSHOT_SIZE \
    (DEFY_BATTERY_SNAPSHOT_HEADER + 2u * DEFY_BATTERY_SNAPSHOT_RECORD_SIZE)

struct defy_battery_record {
    struct defy_battery_frame frame;
    struct defy_battery_reading reading;
    bool present;
    bool connected;
    bool fresh;
    bool valid;
    /* The links the half's frame is live on, BIT(enum defy_link). */
    uint8_t route_mask;
    int validation_error;
};

/*
 * A route is a link and a source on it: the cable port, or the Bluetooth
 * split slot. A side is DEFY_WIRED_LEFT or DEFY_WIRED_RIGHT, or, where a
 * route's half is not known yet, DEFY_WIRED_SIDE_UNKNOWN.
 */
int defy_battery_central_route_live(enum defy_link route, uint8_t source, uint32_t epoch,
                                    uint8_t side);
int defy_battery_central_route_identify(enum defy_link route, uint8_t source, uint32_t epoch,
                                        uint8_t side);
int defy_battery_central_route_side(enum defy_link route, uint8_t source, uint32_t epoch,
                                    uint8_t *side);
void defy_battery_central_disconnect(enum defy_link route, uint8_t source, uint32_t epoch);
/*
 * Takes a frame that came by a route, aged extra_age_ms more than it says.
 * -EBADMSG for a frame that does not decode, -ENOTCONN for a route that is
 * down or of another epoch, -EPROTO for the other half's frame, -EADDRINUSE
 * when the link's other source holds that half, -ESTALE for an older sample.
 */
int defy_battery_central_accept(enum defy_link route, uint8_t source, uint32_t epoch,
                                const uint8_t *data, size_t length, uint32_t extra_age_ms);
int defy_battery_central_get(uint8_t side, struct defy_battery_record *record);
int defy_battery_central_aggregate(uint8_t *level, uint32_t *revision);
int defy_battery_central_diagnostic(uint8_t side,
                                    uint8_t data[DEFY_BATTERY_DIAGNOSTIC_SIZE]);
size_t defy_battery_central_snapshot(uint8_t *data, size_t capacity);
void defy_battery_host_changed(void);

#endif
