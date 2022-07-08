/*
 * Copyright (c) 2018-2021 mcumgr authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <logging/log.h>
LOG_MODULE_REGISTER(mgmt, 4);

#include <string.h>

#include "tinycbor/cbor.h"
#include "mgmt/endian.h"
#include "mgmt/mgmt.h"
#include <sys/atomic.h>

static sys_slist_t mgmt_event_callback_list;

static struct mgmt_group *server_list_head;
static struct mgmt_group *server_list_end;

static struct mgmt_group *client_list_head;
static struct mgmt_group *client_list_end;

#ifdef CONFIG_MCUMGR_CLIENT
static atomic_t mgmt_sequence = ATOMIC_INIT(0);
#endif


void *
mgmt_streamer_alloc_rsp(struct mgmt_streamer *streamer, const void *req)
{
	return streamer->cfg->alloc_rsp(req, streamer->cb_arg);
}

void
mgmt_streamer_trim_front(struct mgmt_streamer *streamer, void *buf, size_t len)
{
	streamer->cfg->trim_front(buf, len, streamer->cb_arg);
}

void
mgmt_streamer_reset_buf(struct mgmt_streamer *streamer, void *buf)
{
	streamer->cfg->reset_buf(buf, streamer->cb_arg);
}

int
mgmt_streamer_write_at(struct mgmt_streamer *streamer, size_t offset,
					   const void *data, int len)
{
	return streamer->cfg->write_at(streamer->writer, offset, data, len, streamer->cb_arg);
}

int
mgmt_streamer_init_reader(struct mgmt_streamer *streamer, void *buf)
{
	return streamer->cfg->init_reader(streamer->reader, buf, streamer->cb_arg);
}

int
mgmt_streamer_init_writer(struct mgmt_streamer *streamer, void *buf)
{
	return streamer->cfg->init_writer(streamer->writer, buf, streamer->cb_arg);
}

void
mgmt_streamer_free_buf(struct mgmt_streamer *streamer, void *buf)
{
	streamer->cfg->free_buf(buf, streamer->cb_arg);
}

void
mgmt_unregister_group(struct mgmt_group *group)
{
	struct mgmt_group *curr = server_list_head, *prev = NULL;

	if (!group) {
		return;
	}

	if (curr == group) {
		server_list_head = curr->mg_next;
		return;
	}

	while (curr && curr != group) {
		prev = curr;
		curr = curr->mg_next;
	}

	if (!prev || !curr) {
		return;
	}

	prev->mg_next = curr->mg_next;
	if (curr->mg_next == NULL) {
		server_list_end = curr;
	}
}

static struct mgmt_group *
mgmt_find_group(uint16_t group_id, uint16_t command_id, bool client)
{
	struct mgmt_group *group;

	group = client ? client_list_head : server_list_head;

	/*
	 * Find the group with the specified group id, if one exists
	 * check the handler for the command id and make sure
	 * that is not NULL. If that is not set, look for the group
	 * with a command id that is set
	 */
	for (; group != NULL; group = group->mg_next) {
		if (group->mg_group_id == group_id) {
			if (command_id >= group->mg_handlers_count) {
				return NULL;
			}

			if (!group->mg_handlers[command_id].mh_read &&
			    !group->mg_handlers[command_id].mh_write) {
				continue;
			}

			break;
		}
	}

	return group;
}

void
mgmt_register_group(struct mgmt_group *group)
{
	if (server_list_end == NULL) {
		server_list_head = group;
	} else {
		server_list_end->mg_next = group;
	}
	server_list_end = group;
}

#ifdef CONFIG_MCUMGR_CLIENT
void
mgmt_register_client_group(struct mgmt_group *group)
{
	if (client_list_end == NULL) {
		client_list_head = group;
	} else {
		client_list_end->mg_next = group;
	}
	client_list_end = group;
}
#endif

const struct mgmt_handler *
mgmt_find_handler(uint16_t group_id, uint16_t command_id)
{
	const struct mgmt_group *group;

	group = mgmt_find_group(group_id, command_id, false);
	if (!group) {
		return NULL;
	}

	return &group->mg_handlers[command_id];
}

const struct mgmt_handler *
mgmt_find_client_handler(uint16_t group_id, uint16_t command_id)
{
	const struct mgmt_group *group;

	group = mgmt_find_group(group_id, command_id, true);
	if (!group) {
		return NULL;
	}

	return &group->mg_handlers[command_id];
}

int
mgmt_write_rsp_status(struct mgmt_ctxt *ctxt, int errcode)
{
	int rc;

	rc = cbor_encode_text_stringz(&ctxt->encoder, "rc");
	if (rc != 0) {
		return rc;
	}

	rc = cbor_encode_int(&ctxt->encoder, errcode);
	if (rc != 0) {
		return rc;
	}

	return 0;
}

int
mgmt_err_from_cbor(int cbor_status)
{
	switch (cbor_status) {
	case CborNoError:
		return MGMT_ERR_EOK;
	case CborErrorOutOfMemory:
		return MGMT_ERR_ENOMEM;
	}
	return MGMT_ERR_EUNKNOWN;
}

int
mgmt_ctxt_init(struct mgmt_ctxt *ctxt, struct mgmt_streamer *streamer,
		   const struct mgmt_hdr *hdr)
{
	int rc;

	rc = cbor_parser_init(streamer->reader, 0, &ctxt->parser, &ctxt->it);
	if (rc != CborNoError) {
		return mgmt_err_from_cbor(rc);
	}

	cbor_encoder_init(&ctxt->encoder, streamer->writer, 0);

	memcpy(&ctxt->hdr, hdr, sizeof(ctxt->hdr));

	return 0;
}

void
mgmt_ntoh_hdr(struct mgmt_hdr *hdr)
{
	hdr->nh_len = ntohs(hdr->nh_len);
	hdr->nh_group = ntohs(hdr->nh_group);
}

void
mgmt_hton_hdr(struct mgmt_hdr *hdr)
{
	hdr->nh_len = htons(hdr->nh_len);
	hdr->nh_group = htons(hdr->nh_group);
}

int
mgmt_register_evt_cb(struct mgmt_on_evt_cb_entry *entry)
{
	sys_slist_append(&mgmt_event_callback_list, &entry->node);

	return 0;
}

void
mgmt_evt(uint8_t opcode, const struct mgmt_hdr *hdr, void *arg)
{
	sys_snode_t *node;
	struct mgmt_on_evt_cb_entry *entry;

#ifdef CONFIG_MCUMGR_STATUS_STRINGS
	int status = 0;

	if (arg) {
		switch (opcode) {
		case MGMT_EVT_OP_CMD_SENT:
			status = ((struct mgmt_evt_op_cmd_sent_arg *)arg)->err;
			break;
		case MGMT_EVT_OP_CLIENT_DONE:
			status = ((struct mgmt_evt_op_cmd_done_arg *)arg)->err;
			break;
		default:
			status = 0;
			break;
		}
	}

	LOG_DBG("%s %s %s group: %u id: %u seq: %u status: %s",
		(hdr->nh_flags & MGMT_FLAG_NOTIFICATION) ? "[Notification]" : "",
		mgmt_get_string_operation(hdr->nh_op), mgmt_get_string_event(opcode), hdr->nh_group,
		hdr->nh_id, hdr->nh_seq, mgmt_get_string_err(status));
#endif

	SYS_SLIST_FOR_EACH_NODE(&mgmt_event_callback_list, node) {
    	entry = CONTAINER_OF(node, struct mgmt_on_evt_cb_entry, node);
		if (entry->cb != NULL) {
			entry->cb(opcode, hdr, arg);
		}
	}
}


bool
is_cmd(const struct mgmt_hdr *hdr)
{
    return ((hdr->nh_op % 2) == 0);
}

#ifdef CONFIG_MCUMGR_CLIENT
uint8_t
mgmt_get_sequence(void)
{
	return (uint8_t)atomic_inc(&mgmt_sequence);
}

void mgmt_generate_cmd_sent_event(struct mgmt_hdr *nwk_hdr, int err)
{
	struct mgmt_hdr host_hdr;
	struct mgmt_evt_op_cmd_sent_arg arg;

	memcpy(&host_hdr, nwk_hdr, sizeof(host_hdr));
	mgmt_ntoh_hdr(&host_hdr);
	arg.err = err;
	mgmt_evt(MGMT_EVT_OP_CMD_SENT, &host_hdr, &err);
}
#endif

#ifdef CONFIG_MCUMGR_STATUS_STRINGS

const char *
mgmt_get_string_event(uint8_t event)
{
	switch (event) {
	case MGMT_EVT_OP_CMD_RECV:
		return "Cmd Recv";
	case MGMT_EVT_OP_CMD_STATUS:
		return "Cmd Status";
	case MGMT_EVT_OP_CMD_DONE:
		return "Cmd Done";
	case MGMT_EVT_OP_CLIENT_RECV:
		return "Client Recv";
	case MGMT_EVT_OP_CLIENT_STATUS:
		return "Client Status";
	case MGMT_EVT_OP_CLIENT_DONE:
		return "Client Done";
	case MGMT_EVT_OP_CMD_SENT:
		return "Cmd Sent";
	default:
		return "?";
	}
}

const char *
mgmt_get_string_operation(uint8_t operation)
{
	switch (operation) {
	case MGMT_OP_READ:
		return "Read";
	case MGMT_OP_READ_RSP:
		return "Read Rsp";
	case MGMT_OP_WRITE:
		return "Write";
	case MGMT_OP_WRITE_RSP:
		return "Write Rsp";
	default:
		return "?";
	}
}

const char *
mgmt_get_string_group(uint16_t group)
{
	switch (group) {
	case MGMT_GROUP_ID_OS:
		return "OS";
	case MGMT_GROUP_ID_IMAGE:
		return "Image";
	case MGMT_GROUP_ID_STAT:
		return "Stat";
	case MGMT_GROUP_ID_CONFIG:
		return "Config";
	case MGMT_GROUP_ID_LOG:
		return "Log";
	case MGMT_GROUP_ID_CRASH:
		return "Crash";
	case MGMT_GROUP_ID_SPLIT:
		return "Split";
	case MGMT_GROUP_ID_RUN:
		return "Run";
	case MGMT_GROUP_ID_FS:
		return "FS";
	case MGMT_GROUP_ID_SHELL:
		return "Shell";
	default:
		return "?";
	}
}

const char *
mgmt_get_string_err(int err)
{
	switch (err) {
	case MGMT_ERR_EOK:
		return "OK";
	case MGMT_ERR_EUNKNOWN:
		return "Unknown";
	case MGMT_ERR_ENOMEM:
		return "No Memory";
	case MGMT_ERR_EINVAL:
		return "Invalid Argument";
	case MGMT_ERR_ETIMEOUT:
		return "Timeout";
	case MGMT_ERR_ENOENT:
		return "No Entry";
	case MGMT_ERR_EBADSTATE:
		return "Current state disallows command";
	case MGMT_ERR_EMSGSIZE:
		return "Message too large";
	case MGMT_ERR_ENOTSUP:
		return "Command not supported";
	case MGMT_ERR_ECORRUPT:
		return "Corrupt";
	case MGMT_ERR_NO_CLIENT:
		return "Client handler not found";
	case MGMT_ERR_DECODE:
		return "Decode";
	case MGMT_ERR_ENCODE:
		return "Encode";
	case MGMT_ERR_OFFSET:
		return "Offset";
	case MGMT_ERR_TRANSPORT:
		return "Transport";
	case MGMT_ERR_BUSY:
		return "Busy";
	case MGMT_ERR_WRITE:
		return "Write Transport";
	case MGMT_ERR_OPEN:
		return "Unable to open transport";
	case MGMT_ERR_CLOSE:
		return "Unable to close transport";
	case MGMT_ERR_LENGTH_MISSING:
		return "Length missing from first chunk of file operation";
	case MGMT_ERR_NOTIFICATION_TIMEOUT:
		return "Notification Receive Timeout";
	case MGMT_ERR_NOTIFICATION_EXPECTED:
		return "Notification Expected";
	default:
		return "?";
	}
}

#endif /* CONFIG_MCUMGR_STATUS_STRINGS */