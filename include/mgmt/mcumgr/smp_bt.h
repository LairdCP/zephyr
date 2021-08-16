/*
 * Copyright Runtime.io 2018. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file
 * @brief Bluetooth transport for the mcumgr SMP protocol.
 */

#ifndef ZEPHYR_INCLUDE_MGMT_SMP_BT_H_
#define ZEPHYR_INCLUDE_MGMT_SMP_BT_H_

#include <zephyr/types.h>
struct bt_conn;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Registers the SMP Bluetooth service.
 *
 * @return 0 on success; negative error code on failure.
 */
int smp_bt_register(void);

/**
 * @brief Unregisters the SMP Bluetooth service.
 *
 * @return 0 on success; negative error code on failure.
 */
int smp_bt_unregister(void);

/**
 * Transmits an SMP command/response over the specified Bluetooth connection
 * as a notification.
 *
 * @param conn Connection object.
 * @param data Pointer to Attribute data.
 * @param len Attribute value length.
 *
 * @return 0 in case of success or negative value in case of error.
 */
int smp_bt_tx(struct bt_conn *conn, const void *data, uint16_t len)

#ifdef __cplusplus
}
#endif

#endif
