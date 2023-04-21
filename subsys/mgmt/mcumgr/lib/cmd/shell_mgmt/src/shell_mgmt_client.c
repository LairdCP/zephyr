/**
 * @file shell_mgmt_client.c
 * @brief
 *
 * Copyright (c) 2023 Laird Connectivity
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <logging/log.h>
LOG_MODULE_REGISTER(mcumgr_client_shell, CONFIG_MCUMGR_CLIENT_SHELL_LOG_LEVEL);

/**************************************************************************************************/
/* Includes                                                                                       */
/**************************************************************************************************/
#include <zephyr.h>
#include <init.h>
#include <sys/printk.h>
#include <mgmt/endian.h>

#include "shell_exec_cmd_encode.h"
#include "shell_exec_rsp_decode.h"
#include "zcbor_mgmt.h"

#include "shell_mgmt/shell_mgmt_impl.h"
#include "shell_mgmt/shell_mgmt_config.h"
#include "shell_mgmt/shell_mgmt.h"
#include "shell_mgmt/shell_mgmt_client.h"

/**************************************************************************************************/
/* Local Function Prototypes                                                                      */
/**************************************************************************************************/
static int shell_exec_rsp_handler(struct mgmt_ctxt *ctxt);

static void shell_mgmt_event_callback(uint8_t event, const struct mgmt_hdr *hdr, void *arg);

/**************************************************************************************************/
/* Local Constant, Macro and Type Definitions                                                     */
/**************************************************************************************************/
#define BUILD_NETWORK_HEADER(op, len, id) SET_NETWORK_HEADER(op, len, MGMT_GROUP_ID_SHELL, id)

/* clang-format off */
static const struct mgmt_handler SHELL_MGMT_CLIENT_HANDLERS[] = {
	[SHELL_MGMT_ID_EXEC] = {
		.mh_read = shell_exec_rsp_handler,
		.mh_write = shell_exec_rsp_handler
	}
};
/* clang-format on */

#define LOG_ERR_ENCODE(_r)                                                                         \
	LOG_ERR("Unable to encode %s status: %s", __func__, zcbor_get_error_string(_r))

#define LOG_ERR_SERVER_APPLICATION(_r)                                                             \
	LOG_ERR("Server reported shell mgmt error (%s): %d", __func__, _r)

#define DEFAULT_CMD_TIMEOUT CONFIG_MCUMGR_CLIENT_SHELL_MGMT_CMD_TIMEOUT

/**************************************************************************************************/
/* Local Data Definitions                                                                         */
/**************************************************************************************************/
static struct mgmt_group shell_mgmt_client_group = {
	.mg_handlers = SHELL_MGMT_CLIENT_HANDLERS,
	.mg_handlers_count =
		(sizeof SHELL_MGMT_CLIENT_HANDLERS / sizeof SHELL_MGMT_CLIENT_HANDLERS[0]),
	.mg_group_id = MGMT_GROUP_ID_SHELL,
};

static struct mgmt_on_evt_cb_entry shell_cb_entry;

static K_MUTEX_DEFINE(shell_client);

static struct {
	struct k_sem busy;
	struct mgmt_hdr cmd_hdr;
	uint8_t cmd[CONFIG_MCUMGR_BUF_SIZE];
	int sequence;
	volatile int status;
	int timeout_ms;
	char *output;
	size_t output_max_len;
} shell_ctx;

/**************************************************************************************************/
/* Sys Init                                                                                       */
/**************************************************************************************************/
static int shell_mgmt_client_init(const struct device *device)
{
	ARG_UNUSED(device);

	mgmt_register_client_group(&shell_mgmt_client_group);

	shell_cb_entry.cb = shell_mgmt_event_callback;
	mgmt_register_evt_cb(&shell_cb_entry);

	k_sem_init(&shell_ctx.busy, 0, 1);

	return 0;
}

SYS_INIT(shell_mgmt_client_init, APPLICATION, 99);

/**************************************************************************************************/
/* Local Function Definitions                                                                     */
/**************************************************************************************************/
static int shell_send_cmd(struct zephyr_smp_transport *transport, struct mgmt_hdr *hdr,
			  const void *cbor_data)
{
	k_sem_reset(&shell_ctx.busy);
	shell_ctx.sequence = hdr->nh_seq;
	shell_ctx.status = zephyr_smp_tx_cmd(transport, hdr, cbor_data);

	if (shell_ctx.status == 0) {
		if (k_sem_take(&shell_ctx.busy, K_MSEC(shell_ctx.timeout_ms)) != 0) {
			LOG_ERR("Titan mgmt client timeout");
			shell_ctx.status = MGMT_ERR_ETIMEOUT;
		}
	} else {
		LOG_ERR("Unable to send: %s", mgmt_get_string_err(shell_ctx.status));
	}

	return shell_ctx.status;
}

static int shell_exec_rsp_handler(struct mgmt_ctxt *ctxt)
{
	int r = MGMT_ERR_EBADSTATE;
	struct shell_exec_rsp rsp;

	if (shell_ctx.status != 0) {
		return r;
	}

	if (zcbor_mgmt_decode_client(ctxt, cbor_decode_shell_exec_rsp, &rsp) != 0) {
		LOG_ERR("Decode of response failed");
		r = zcbor_mgmt_decode_err(ctxt);
	} else {
		/* This version of SMP doesn't allow easy differentiation between
		 * SMP errors and command errors. Assume > 0 is SMP error and < 0 is
		 * command error.
		 */
		if (rsp.rc > 0) {
			LOG_ERR_SERVER_APPLICATION(rsp.rc);
			return rsp.rc;
		} else {
			r = rsp.rc;
		}

		/* copy output of command */
		if (rsp.o.len < shell_ctx.output_max_len) {
			memcpy(shell_ctx.output, rsp.o.value, rsp.o.len);
			shell_ctx.output[rsp.o.len] = 0;
		} else {
			LOG_ERR("Shell exec output string too small: %u < %u",
				shell_ctx.output_max_len, rsp.o.len);
			r = MGMT_ERR_ENOMEM;
		}
	}

	return r;
}

static void shell_mgmt_event_callback(uint8_t event, const struct mgmt_hdr *hdr, void *arg)
{
	/* Short circuit if event is a don't care */
	if (is_cmd(hdr) || (hdr->nh_group != MGMT_GROUP_ID_SHELL)) {
		return;
	}

	if (event == MGMT_EVT_OP_CLIENT_DONE) {
		if (hdr->nh_seq != shell_ctx.sequence) {
			LOG_DBG("Unexpected sequence");
		}

		shell_ctx.status =
			arg ? ((struct mgmt_evt_op_cmd_done_arg *)arg)->err : MGMT_ERR_EUNKNOWN;

		k_sem_give(&shell_ctx.busy);
	}
}

/**************************************************************************************************/
/* Global Function Definitions                                                                    */
/**************************************************************************************************/
int shell_mgmt_client_exec(struct zephyr_smp_transport *transport, const char *command,
			   size_t length, char *output, size_t output_size)
{
	int r;
	size_t cmd_len = 0;
	struct shell_exec_cmd cmd;

	if (transport == NULL || command == NULL || length == 0 || output == NULL) {
		return MGMT_ERR_EINVAL;
	}

	r = k_mutex_lock(&shell_client, K_NO_WAIT);
	if (r < 0) {
		return MGMT_ERR_BUSY;
	}

	cmd.args[0].value = command;
	cmd.args[0].len = length;
	cmd.args_count = 1;
	r = cbor_encode_shell_exec_cmd(shell_ctx.cmd, sizeof(shell_ctx.cmd), &cmd, &cmd_len);
	if (r != 0) {
		LOG_ERR_ENCODE(r);
		return MGMT_ERR_ENCODE;
	}
	shell_ctx.cmd_hdr = BUILD_NETWORK_HEADER(MGMT_OP_WRITE, cmd_len, SHELL_MGMT_ID_EXEC);
	shell_ctx.timeout_ms = DEFAULT_CMD_TIMEOUT;

	shell_ctx.status = 0;
	shell_ctx.output = output;
	shell_ctx.output_max_len = (output_size > 0) ? (output_size - 1) : 0;
	memset(output, 0, output_size);

	r = shell_send_cmd(transport, &shell_ctx.cmd_hdr, shell_ctx.cmd);

	k_mutex_unlock(&shell_client);
	return r;
}
