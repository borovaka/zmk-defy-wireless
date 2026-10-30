/*
 * CONFIG_BT_MAX_CONN connections: connection(i) is the address of token i,
 * which the module sources take for a struct bt_conn, and bt_conn_index()
 * gives i back. bt_conn_ref() and bt_conn_unref() count each connection's
 * references in refs[].
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DEFY_TEST_FAKE_CONN_H
#define DEFY_TEST_FAKE_CONN_H

#include <stddef.h>
#include <stdint.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

static uint32_t tokens[CONFIG_BT_MAX_CONN];
static int refs[CONFIG_BT_MAX_CONN];

static struct bt_conn *connection(size_t index) { return (struct bt_conn *)&tokens[index]; }

uint8_t bt_conn_index(const struct bt_conn *conn) {
    for (size_t i = 0; i < ARRAY_SIZE(tokens); i++) {
        if (conn == connection(i)) {
            return i;
        }
    }
    zassert_unreachable();
    return 0;
}

struct bt_conn *bt_conn_ref(struct bt_conn *conn) {
    refs[bt_conn_index(conn)]++;
    return conn;
}

void bt_conn_unref(struct bt_conn *conn) {
    zassert_true(refs[bt_conn_index(conn)] > 0);
    refs[bt_conn_index(conn)]--;
}

#endif
