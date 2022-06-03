/*
 * Copyright (c) 2018-2022 mcumgr authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef H_FS_MGMT_CLIENT_
#define H_FS_MGMT_CLIENT_

#ifdef __cplusplus
extern "C" {
#endif

#include <zephyr.h>
#include <mgmt/mgmt.h>
#include <mgmt/mcumgr/smp.h>

/**
 * @brief Read a remote file into a buffer
 *
 * @param transport to send message on
 * @param name of file to read from mgmt server
 * @param data location to write data
 * @param size max size of data (input), actual size of data (output)
 * @param offset offset into file to start, offset into file when done
 * @return int 0 on success, MGMT_ERR_[...] code on failure
 */
int fs_mgmt_client_download(struct zephyr_smp_transport *transport, const char *name,
			  void *data, size_t *size, size_t *offset);

/**
 * @brief Read a remote file into a local file
 *
 * @param transport to send message on
 * @param remote_name of file to read from mgmt server
 * @param local_name of file to write
 * @param size max size of data (input), if zero, then no limit; actual size of data (output)
 * @param offset offset into file to start, offset into file when done
 * @return int 0 on success, MGMT_ERR_[...] code on failure
 */
int fs_mgmt_client_download_file(struct zephyr_smp_transport *transport, const char *remote_name,
				 const char *local_name, size_t *size, size_t *offset);

/**
 * @brief Write a file
 *
 * @param transport to send message on
 * @param name of file to write to mgmt server
 * @param data to write
 * @param size of data
 * @param offset offset into file to start, offset into file when done
 * @return int 0 on success, MGMT_ERR_[...] code on failure
 */
int fs_mgmt_client_upload(struct zephyr_smp_transport *transport, const char *name,
			  const void *data, size_t size, size_t *offset);

/**
 * @brief Write a file from the file system.
 *
 * @param transport to send message on
 * @param name of file to write to mgmt server
 * @param local_name of file to read
 * @param offset offset into file to start, offset into file when done
 * @return int 0 on success, MGMT_ERR_[...] code on failure
 */
int fs_mgmt_client_upload_file(struct zephyr_smp_transport *transport, const char *name,
			       const char *local_name, size_t *offset);

#ifdef __cplusplus
}
#endif

#endif
