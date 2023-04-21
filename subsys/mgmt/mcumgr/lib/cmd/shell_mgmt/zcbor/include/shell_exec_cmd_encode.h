/*
 * Generated using zcbor version 0.6.0
 * https://github.com/NordicSemiconductor/zcbor
 * Generated with a --default-max-qty of 12
 */

#ifndef SHELL_EXEC_CMD_ENCODE_H__
#define SHELL_EXEC_CMD_ENCODE_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "zcbor_encode.h"
#include "shell_exec_cmd_types.h"

#if DEFAULT_MAX_QTY != 12
#error "The type file was generated with a different default_max_qty than this file"
#endif


int cbor_encode_shell_exec_cmd(
		uint8_t *payload, size_t payload_len,
		const struct shell_exec_cmd *input,
		size_t *payload_len_out);


#endif /* SHELL_EXEC_CMD_ENCODE_H__ */
