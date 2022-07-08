/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#include <logging/log.h>
LOG_MODULE_REGISTER(os_mgmt_client, CONFIG_MGMT_CLIENT_OS_LOG_LEVEL);

#include <init.h>
#include <string.h>

#include <mgmt/endian.h>

#include "echo_cmd_encode.h"
#include "echo_rsp_decode.h"
#include "zcbor_mgmt.h"

#include "os_mgmt/os_mgmt.h"
#include "os_mgmt/os_mgmt_impl.h"
#include "os_mgmt/os_mgmt_config.h"
#include "os_mgmt/os_mgmt_client.h"

#define BUILD_NETWORK_HEADER(op, len, id) SET_NETWORK_HEADER(op, len, MGMT_GROUP_ID_OS, id)

#define CONFIG_OS_MGMT_CLIENT_CMD_TIMEOUT_MS 1000

#if CONFIG_OS_MGMT_CLIENT_ECHO
static int os_mgmt_client_echo_handler(struct mgmt_ctxt *ctxt);
#endif

#if CONFIG_OS_MGMT_CLIENT_RESET
static int os_mgmt_client_reset_handler(struct mgmt_ctxt *ctxt);
#endif

#if CONFIG_OS_MGMT_CLIENT_TASKSTAT
static int os_mgmt_client_taskstat_handler(struct mgmt_ctxt *ctxt);
#endif

/* clang-format off */
static const struct mgmt_handler os_mgmt_client_group_handlers[] = {
#if CONFIG_OS_MGMT_CLIENT_ECHO
	[OS_MGMT_ID_ECHO] = {
		.mh_read = os_mgmt_client_echo_handler,
		.mh_write = os_mgmt_client_echo_handler
	},
#endif
#if CONFIG_OS_MGMT_CLIENT_TASKSTAT
	[OS_MGMT_ID_TASKSTAT] = {
		.mh_read = os_mgmt_client_taskstat_handler,
		.mh_write = NULL
	},
#endif
#if CONFIG_OS_MGMT_CLIENT_RESET
	[OS_MGMT_ID_RESET] = {
		.mh_read = NULL,
		.mh_write = os_mgmt_client_reset_handler
	},
#endif
};
/* clang-format on */

#define OS_MGMT_CLIENT_GROUP_SIZE                                                                  \
	(sizeof os_mgmt_client_group_handlers / sizeof os_mgmt_client_group_handlers[0])

static struct mgmt_group os_mgmt_client_group = {
	.mg_handlers = os_mgmt_client_group_handlers,
	.mg_handlers_count = OS_MGMT_CLIENT_GROUP_SIZE,
	.mg_group_id = MGMT_GROUP_ID_OS,
};

static struct mgmt_on_evt_cb_entry os_cb_entry;

static K_MUTEX_DEFINE(os_client);

static struct {
	struct k_sem busy;
	int sequence;
	volatile int status;
	struct echo_cmd echo_cmd;
} os_client_context;

static int os_send_cmd(struct zephyr_smp_transport *transport, struct mgmt_hdr *hdr,
		       const void *cbor_data)
{
	int r;

	os_client_context.sequence = hdr->nh_seq;
	r = zephyr_smp_tx_cmd(transport, hdr, cbor_data);

	if (r == 0) {
		if (k_sem_take(&os_client_context.busy,
			       K_MSEC(CONFIG_OS_MGMT_CLIENT_CMD_TIMEOUT_MS)) != 0) {
			r = MGMT_ERR_ETIMEOUT;
		} else {
			r = os_client_context.status;
		}
	}

	return r;
}

static void os_mgmt_event_callback(uint8_t event, const struct mgmt_hdr *hdr, void *arg)
{
	/* Short circuit if event is a don't care */
	if (is_cmd(hdr) || (hdr->nh_group != MGMT_GROUP_ID_OS)) {
		return;
	}

	if (event == MGMT_EVT_OP_CLIENT_DONE) {
		if (hdr->nh_seq != os_client_context.sequence) {
			LOG_DBG("Unexpected sequence");
		}

		os_client_context.status =
			arg ? ((struct mgmt_evt_op_cmd_done_arg *)arg)->err : MGMT_ERR_EUNKNOWN;

		k_sem_give(&os_client_context.busy);
	}
}

int os_mgmt_client_echo(struct zephyr_smp_transport *transport, const char *msg, mgmt_seq_cb cb)
{
	int r;
	int msg_length;
	struct mgmt_hdr cmd_hdr;
	size_t cmd_len = 0;
	uint8_t cmd[CONFIG_MCUMGR_BUF_SIZE];

	if (msg == NULL) {
		return MGMT_ERR_EINVAL;
	}

	msg_length = strlen(msg);
	if (msg_length == 0 || msg_length > CONFIG_MCUMGR_BUF_SIZE) {
		return MGMT_ERR_EINVAL;
	}

	r = k_mutex_lock(&os_client, K_NO_WAIT);
	if (r < 0) {
		return MGMT_ERR_BUSY;
	}

	os_client_context.status = 0;

	os_client_context.echo_cmd.d.value = msg;
	os_client_context.echo_cmd.d.len = msg_length;

	if (cbor_encode_echo_cmd(cmd, sizeof(cmd), &os_client_context.echo_cmd, &cmd_len)) {
		LOG_ERR("Unable to encode %s", __func__);
		return MGMT_ERR_ENCODE;
	}

	cmd_hdr = BUILD_NETWORK_HEADER(MGMT_OP_READ, cmd_len, OS_MGMT_ID_ECHO);

	if (cb != NULL) {
		cb(cmd_hdr.nh_seq);
	}

	LOG_HEXDUMP_DBG(msg, msg_length, "Echo cmd");

	r = os_send_cmd(transport, &cmd_hdr, cmd);

	os_client_context.echo_cmd.d.value = NULL;
	k_mutex_unlock(&os_client);
	return r;
}

#if CONFIG_OS_MGMT_CLIENT_ECHO
static int os_mgmt_client_echo_handler(struct mgmt_ctxt *ctxt)
{
	struct echo_rsp rsp;

	if (os_client_context.status != 0 || os_client_context.echo_cmd.d.value == NULL) {
		return MGMT_ERR_EBADSTATE;
	}

	if (zcbor_mgmt_decode_client(ctxt, cbor_decode_echo_rsp, &rsp) != 0) {
		return zcbor_mgmt_decode_err(ctxt);
	}

	/* Response isn't terminated */
	LOG_HEXDUMP_DBG(rsp.r.value, rsp.r.len, "Echo Response");

	if (rsp.r.len == os_client_context.echo_cmd.d.len) {
		if (memcmp(rsp.r.value, os_client_context.echo_cmd.d.value, rsp.r.len) == 0) {
			return MGMT_ERR_EOK;
		}
	}

	return MGMT_ERR_EPERUSER;
}
#endif

#if CONFIG_OS_MGMT_CLIENT_TASKSTAT
static int os_mgmt_client_taskstat_handler(struct mgmt_ctxt *ctxt)
{
	return MGMT_ERR_NO_CLIENT;
}
#endif

#if CONFIG_OS_MGMT_CLIENT_RESET
static int os_mgmt_client_reset_handler(struct mgmt_ctxt *ctxt)
{
	return MGMT_ERR_NO_CLIENT;
}
#endif

static int os_mgmt_client_init(const struct device *device)
{
	ARG_UNUSED(device);

	mgmt_register_client_group(&os_mgmt_client_group);

	os_cb_entry.cb = os_mgmt_event_callback;
	mgmt_register_evt_cb(&os_cb_entry);

	k_sem_init(&os_client_context.busy, 0, 1);

	return 0;
}

SYS_INIT(os_mgmt_client_init, APPLICATION, 99);
