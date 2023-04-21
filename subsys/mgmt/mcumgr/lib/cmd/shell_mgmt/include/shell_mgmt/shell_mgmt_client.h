/*
 * Copyright (c) 2018-2023 mcumgr authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef H_SHELL_MGMT_CLIENT_
#define H_SHELL_MGMT_CLIENT_

#ifdef __cplusplus
extern "C" {
#endif

#include <zephyr.h>
#include <mgmt/mgmt.h>
#include <mgmt/mcumgr/smp.h>

/**
 * @brief Execute a shell command on a remote device
 *
 * @param transport to send message on
 * @param command string (doesn't need to be null terminated)
 * @param length of string
 * @param output string (is null terminated)
 * @param output_size size of output string
 * @return int 0 on success, MGMT_ERR_[...] code on SMP failure, negative if
 * command returns error from remote.
 */
int shell_mgmt_client_exec(struct zephyr_smp_transport *transport, const char *command,
			   size_t length, char *output, size_t output_size);

#ifdef __cplusplus
}
#endif

#endif
