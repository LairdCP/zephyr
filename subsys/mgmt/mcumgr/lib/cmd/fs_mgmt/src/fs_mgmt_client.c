/**
 * @file fs_mgmt_client.c
 * @brief
 *
 * Copyright (c) 2022 Laird Connectivity
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <logging/log.h>
LOG_MODULE_REGISTER(mcumgr_client_fs, CONFIG_MCUMGR_CLIENT_FS_LOG_LEVEL);

/**************************************************************************************************/
/* Includes                                                                                       */
/**************************************************************************************************/
#include <zephyr.h>
#include <init.h>
#include <sys/printk.h>
#include <mgmt/endian.h>

#include "file_download_cmd_encode.h"
#include "file_download_rsp_decode.h"
#include "file_upload_cmd_encode.h"
#include "file_upload_rsp_decode.h"
#include "zcbor_mgmt.h"

#include "fs_mgmt/fs_mgmt_impl.h"
#include "fs_mgmt/fs_mgmt_config.h"
#include "fs_mgmt/fs_mgmt.h"
#include "fs_mgmt/fs_mgmt_client.h"

/**************************************************************************************************/
/* Local Function Prototypes                                                                      */
/**************************************************************************************************/
static int download_rsp_handler(struct mgmt_ctxt *ctxt);
static int upload_rsp_handler(struct mgmt_ctxt *ctxt);

static void fs_mgmt_event_callback(uint8_t event, const struct mgmt_hdr *hdr, void *arg);

/**************************************************************************************************/
/* Local Constant, Macro and Type Definitions                                                     */
/**************************************************************************************************/
#define MCUMGR_OVERHEAD (CBOR_AND_OTHER_HDR + 32)

/* Should be read from transport, but
 * if client is busy or slow to write fs then it shouldn't timeout either.
 */
#define CONFIG_FS_CMD_TIMEOUT_MS 3000

#define BUILD_NETWORK_HEADER(op, len, id) SET_NETWORK_HEADER(op, len, MGMT_GROUP_ID_FS, id)

/* clang-format off */
static const struct mgmt_handler FS_MGMT_CLIENT_HANDLERS[] = {
	[FS_MGMT_ID_FILE] = {
		.mh_read = download_rsp_handler,
		.mh_write = upload_rsp_handler
	}
};
/* clang-format on */

/**************************************************************************************************/
/* Local Data Definitions                                                                         */
/**************************************************************************************************/
static struct mgmt_group fs_mgmt_client_group = {
	.mg_handlers = FS_MGMT_CLIENT_HANDLERS,
	.mg_handlers_count = (sizeof FS_MGMT_CLIENT_HANDLERS / sizeof FS_MGMT_CLIENT_HANDLERS[0]),
	.mg_group_id = MGMT_GROUP_ID_FS,
};

static struct mgmt_on_evt_cb_entry fs_cb_entry;

static K_MUTEX_DEFINE(fs_client);

static struct {
	struct k_sem busy;
	struct mgmt_hdr cmd_hdr;
	uint8_t cmd[CONFIG_MCUMGR_BUF_SIZE];
	int sequence;
	bool first_chunk;
	volatile int status;
	volatile size_t offset;
	volatile size_t size;
	size_t chunk_size;
	uint8_t *data;
	const char *name;
	const char *local_name;
} fs_ctx;

/**************************************************************************************************/
/* Sys Init                                                                                       */
/**************************************************************************************************/
static int fs_mgmt_client_init(const struct device *device)
{
	ARG_UNUSED(device);

	mgmt_register_client_group(&fs_mgmt_client_group);

	fs_cb_entry.cb = fs_mgmt_event_callback;
	mgmt_register_evt_cb(&fs_cb_entry);

	k_sem_init(&fs_ctx.busy, 0, 1);

	return 0;
}

SYS_INIT(fs_mgmt_client_init, APPLICATION, 99);

/**************************************************************************************************/
/* Local Function Definitions                                                                     */
/**************************************************************************************************/
static int fs_send_cmd(struct zephyr_smp_transport *transport, struct mgmt_hdr *hdr,
		       const void *cbor_data)
{
	int r;

	fs_ctx.sequence = hdr->nh_seq;
	r = zephyr_smp_tx_cmd(transport, hdr, cbor_data);

	return r;
}

static int download_rsp_handler(struct mgmt_ctxt *ctxt)
{
	int r = MGMT_ERR_EBADSTATE;
	struct file_download_rsp rsp;

	if (fs_ctx.status != 0) {
		return r;
	}

	if (zcbor_mgmt_decode_client(ctxt, cbor_decode_file_download_rsp, &rsp) != 0) {
		r = zcbor_mgmt_decode_err(ctxt);
	} else {
		LOG_DBG("rc: %d off: %u", rsp.rc, rsp.offset);

		if (rsp.rc != 0) {
			return rsp.rc;
		}

		/* First chunk must contain length */
		if (fs_ctx.first_chunk) {
			fs_ctx.first_chunk = false;
			if (rsp.len_present) {
				if (rsp.len.len > fs_ctx.size) {
					return MGMT_ERR_ENOMEM;
				} else {
					fs_ctx.size = rsp.len.len;
					LOG_DBG("size: %u", fs_ctx.size);
				}
			} else {
				return MGMT_ERR_LENGTH_MISSING;
			}
		}

		/* Save offset so that it can be used in a retry. */
		fs_ctx.chunk_size = rsp.data.len;
		fs_ctx.offset = rsp.offset;

		if (fs_ctx.local_name != NULL) {
			r = fs_mgmt_impl_write(fs_ctx.local_name, fs_ctx.offset, rsp.data.value,
					       fs_ctx.chunk_size);

		} else {
			if ((fs_ctx.offset + fs_ctx.chunk_size) <= fs_ctx.size) {
				memcpy(fs_ctx.data + fs_ctx.offset, rsp.data.value,
				       fs_ctx.chunk_size);
				r = MGMT_ERR_EOK;
			} else {
				r = MGMT_ERR_OFFSET;
			}
		}

		/* Increment offset for next request */
		if (r == 0) {
			fs_ctx.offset += fs_ctx.chunk_size;
		}
	}

	return r;
}

static int upload_rsp_handler(struct mgmt_ctxt *ctxt)
{
	int r = MGMT_ERR_EBADSTATE;
	struct file_upload_rsp rsp;

	if (fs_ctx.status != 0) {
		return r;
	}

	if (zcbor_mgmt_decode_client(ctxt, cbor_decode_file_upload_rsp, &rsp) != 0) {
		r = zcbor_mgmt_decode_err(ctxt);
	} else {
		LOG_DBG("rc: %d off: %u", rsp.rc, rsp.offset);

		/* An invalid offset is the only response that contains more than the error code */
		if ((rsp.rc == 0) || (rsp.rc == MGMT_ERR_EINVAL)) {
			/* Prepare for next chunk */
			if (rsp.offset <= fs_ctx.size) {
				fs_ctx.offset = rsp.offset;
				fs_ctx.chunk_size =
					MIN(fs_ctx.chunk_size, (fs_ctx.size - fs_ctx.offset));
				r = MGMT_ERR_EOK;
			} else {
				r = MGMT_ERR_OFFSET;
			}
		} else {
			/* FS commands use MGMT error codes for return code */
			r = rsp.rc;
		}
	}

	return r;
}

static void fs_mgmt_event_callback(uint8_t event, const struct mgmt_hdr *hdr, void *arg)
{
	/* Short circuit if event is a don't care */
	if (is_cmd(hdr) || (hdr->nh_group != MGMT_GROUP_ID_FS)) {
		return;
	}

	if (event == MGMT_EVT_OP_CLIENT_DONE) {
		if (hdr->nh_seq != fs_ctx.sequence) {
			LOG_DBG("Unexpected sequence");
		}

		fs_ctx.status =
			arg ? ((struct mgmt_evt_op_cmd_done_arg *)arg)->err : MGMT_ERR_EUNKNOWN;

		k_sem_give(&fs_ctx.busy);
	}
}

static int download_chunk(struct zephyr_smp_transport *transport)
{
	int r;
	size_t cmd_len = 0;
	/* clang-format off */
	struct file_download_cmd file_download_cmd = {
		.offset = fs_ctx.offset,
		.name.value = fs_ctx.name,
		.name.len = strlen(fs_ctx.name)
	};
	/* clang-format on */

	r = cbor_encode_file_download_cmd(fs_ctx.cmd, sizeof(fs_ctx.cmd), &file_download_cmd,
					  &cmd_len);
	if (r != 0) {
		LOG_ERR("Unable to encode fs download chunk %s", zcbor_get_error_string(r));
		return MGMT_ERR_ENCODE;
	}

	fs_ctx.cmd_hdr = BUILD_NETWORK_HEADER(MGMT_OP_READ, cmd_len, FS_MGMT_ID_FILE);
	return fs_send_cmd(transport, &fs_ctx.cmd_hdr, fs_ctx.cmd);
}

static int wait_for_response(void)
{
	if (k_sem_take(&fs_ctx.busy, K_MSEC(CONFIG_FS_CMD_TIMEOUT_MS)) != 0) {
		LOG_ERR("FS Response timeout");
		fs_ctx.status = MGMT_ERR_ETIMEOUT;
	}

	return fs_ctx.status;
}

/* Request file chunks in caller context.
 * Status, data, and offset are updated in a different context.
 */
static int download(struct zephyr_smp_transport *transport)
{
	int r;

	k_sem_reset(&fs_ctx.busy);

	do {
		r = download_chunk(transport);
		if (r == 0) {
			r = wait_for_response();
		}
	} while ((r == 0) && (fs_ctx.offset < fs_ctx.size));

	return r;
}

static int upload_chunk(struct zephyr_smp_transport *transport)
{
	int r;
	size_t cmd_len = 0;
	void *buf = NULL;
	size_t buf_size;

	/* clang-format off */
	struct file_upload_cmd file_upload_cmd = {
		.offset = fs_ctx.offset,
		.len_present = fs_ctx.first_chunk,
		.len.len = fs_ctx.size,
		.name.value = fs_ctx.name,
		.name.len = strlen(fs_ctx.name)
	};
	/* clang-format on */

	do {
		if (fs_ctx.local_name != NULL) {
			buf_size = fs_ctx.chunk_size;
			buf = k_malloc(buf_size);
			if (buf == NULL) {
				r = MGMT_ERR_ENOMEM;
				break;
			}

			r = fs_mgmt_impl_read(fs_ctx.local_name, fs_ctx.offset, buf_size, buf,
					      &fs_ctx.chunk_size);
			if (r != 0) {
				break;
			}
			file_upload_cmd.data.value = buf;
			file_upload_cmd.data.len = fs_ctx.chunk_size;
		} else {
			file_upload_cmd.data.value = fs_ctx.data + fs_ctx.offset;
			file_upload_cmd.data.len = fs_ctx.chunk_size;
		}

		r = cbor_encode_file_upload_cmd(fs_ctx.cmd, sizeof(fs_ctx.cmd), &file_upload_cmd,
						&cmd_len);
		if (r != 0) {
			LOG_ERR("Unable to encode fs upload chunk %s", zcbor_get_error_string(r));
			r = MGMT_ERR_ENCODE;
			break;
		}

		fs_ctx.cmd_hdr = BUILD_NETWORK_HEADER(MGMT_OP_WRITE, cmd_len, FS_MGMT_ID_FILE);
		r = fs_send_cmd(transport, &fs_ctx.cmd_hdr, fs_ctx.cmd);

		fs_ctx.first_chunk = false;

		if (IS_ENABLED(CONFIG_MCUMGR_FS_LOG_VERBOSE)) {
			int i;
			printk("off: %u len: %u\r\n", fs_ctx.offset, file_upload_cmd.data.len);
			for (i = 0; i < file_upload_cmd.data.len; i++) {
				printk("%02x ", file_upload_cmd.data.value[i]);
				if ((i % 16) == 0) {
					printk("\r\n");
				}
			}
			printk("\r\n");
		}

	} while (0);

	k_free(buf);
	return r;
}

/* Send file chunks in caller context.
 * Status and offset are updated in a different context.
 */
static int upload(struct zephyr_smp_transport *transport)
{
	int r;

	k_sem_reset(&fs_ctx.busy);

	do {
		r = upload_chunk(transport);
		if (r == 0) {
			r = wait_for_response();
		}
	} while ((r == 0) && (fs_ctx.offset < fs_ctx.size));

	return r;
}

/* The UL chunk size is currently a compile time constant that must match on
 * both sides of the link.
 */
static int get_chunk_size(struct zephyr_smp_transport *transport)
{
	return MIN(transport->zst_get_mtu(NULL) - MCUMGR_OVERHEAD, CONFIG_FS_MGMT_UL_CHUNK_SIZE);
}

/**************************************************************************************************/
/* Global Function Definitions                                                                    */
/**************************************************************************************************/
int fs_mgmt_client_download(struct zephyr_smp_transport *transport, const char *name, void *data,
			    size_t *size, size_t *offset)
{
	int r;

	if (name == NULL || data == NULL || size == NULL || *size == 0 || offset == NULL ||
	    *offset > *size) {
		return MGMT_ERR_EINVAL;
	}

	r = k_mutex_lock(&fs_client, K_NO_WAIT);
	if (r < 0) {
		return MGMT_ERR_BUSY;
	}

	fs_ctx.first_chunk = true;
	fs_ctx.status = 0;
	fs_ctx.offset = *offset;
	fs_ctx.size = *size; /* max size */
	fs_ctx.name = name;
	fs_ctx.data = (uint8_t *)data;
	fs_ctx.local_name = NULL;

	r = download(transport);

	*size = fs_ctx.size;
	*offset = fs_ctx.offset;
	k_mutex_unlock(&fs_client);
	return r;
}

int fs_mgmt_client_download_file(struct zephyr_smp_transport *transport, const char *remote_name,
				 const char *local_name, size_t *size, size_t *offset)
{
	int r;

	if (remote_name == NULL || local_name == NULL || size == NULL || offset == NULL ||
	    *offset > *size) {
		return MGMT_ERR_EINVAL;
	}

	r = k_mutex_lock(&fs_client, K_NO_WAIT);
	if (r < 0) {
		return MGMT_ERR_BUSY;
	}

	fs_ctx.first_chunk = true;
	fs_ctx.status = 0;
	fs_ctx.offset = *offset;
	fs_ctx.size = (*size == 0) ? SIZE_MAX : *size;
	fs_ctx.name = remote_name;
	fs_ctx.data = 0;
	fs_ctx.local_name = local_name;

	r = download(transport);

	*size = fs_ctx.size;
	*offset = fs_ctx.offset;
	k_mutex_unlock(&fs_client);
	return r;
}

int fs_mgmt_client_upload(struct zephyr_smp_transport *transport, const char *name,
			  const void *data, size_t size, size_t *offset)
{
	int r;
	int chunk_size = get_chunk_size(transport);

	if (chunk_size <= 0) {
		return MGMT_ERR_TRANSPORT;
	}

	if (name == NULL || data == NULL || size == 0 || offset == NULL || *offset > size) {
		return MGMT_ERR_EINVAL;
	}

	r = k_mutex_lock(&fs_client, K_NO_WAIT);
	if (r < 0) {
		return MGMT_ERR_BUSY;
	}

	fs_ctx.first_chunk = true;
	fs_ctx.status = 0;
	fs_ctx.offset = *offset;
	fs_ctx.size = size;
	fs_ctx.chunk_size = MIN(size, chunk_size);
	fs_ctx.name = name;
	fs_ctx.data = (uint8_t *)data;
	fs_ctx.local_name = NULL;

	LOG_DBG("Chunk size: %d", fs_ctx.chunk_size);

	r = upload(transport);

	*offset = fs_ctx.offset;
	k_mutex_unlock(&fs_client);
	return r;
}

int fs_mgmt_client_upload_file(struct zephyr_smp_transport *transport, const char *name,
			       const char *local_name, size_t *offset)
{
	int r;
	size_t size;
	int chunk_size = get_chunk_size(transport);

	if (chunk_size <= 0) {
		return MGMT_ERR_TRANSPORT;
	}

	if (name == NULL || local_name == NULL) {
		return MGMT_ERR_EINVAL;
	}

	r = fs_mgmt_impl_filelen(local_name, &size);
	if (r != 0) {
		return r;
	}

	if (size == 0) {
		return MGMT_ERR_EINVAL;
	}

	r = k_mutex_lock(&fs_client, K_NO_WAIT);
	if (r < 0) {
		return MGMT_ERR_BUSY;
	}

	fs_ctx.first_chunk = true;
	fs_ctx.status = 0;
	fs_ctx.offset = *offset;
	fs_ctx.size = size;
	fs_ctx.chunk_size = MIN(size, chunk_size);
	fs_ctx.name = name;
	fs_ctx.data = 0;
	fs_ctx.local_name = local_name;

	r = upload(transport);

	*offset = fs_ctx.offset;
	k_mutex_unlock(&fs_client);
	return r;
}
