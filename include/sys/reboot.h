/*
 * Copyright (c) 2015 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Common target reboot functionality
 *
 * @details See subsys/os/Kconfig and the reboot help for details.
 */

#ifndef ZEPHYR_INCLUDE_SYS_REBOOT_H_
#define ZEPHYR_INCLUDE_SYS_REBOOT_H_

#include <toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_REBOOT_WARM 0
#define SYS_REBOOT_COLD 1

/**
 * @brief Reboot the system
 *
 * Reboot the system in the manner specified by @a type.  Not all architectures
 * or platforms support the various reboot types (SYS_REBOOT_COLD,
 * SYS_REBOOT_WARM).
 *
 * When successful, this routine does not return.
 *
 * @return N/A
 */

extern FUNC_NORETURN void sys_reboot(int type);

/**
 * @brief Notification that the system is about to reboot. Note that this
 * function should be used for cleaning up or saving data only and should not
 * do anything that may cause a deadlock or take a long time to perform
 *
 * @param type The type of reboot being performed (warm or cold)
 *
 * @return N/A
 */
void sys_reboot_notification(int type);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_SYS_REBOOT_H_ */
