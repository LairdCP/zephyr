/*
 * Generated using zcbor version 0.6.0
 * https://github.com/NordicSemiconductor/zcbor
 * Generated with a --default-max-qty of 12
 */

#ifndef SHELL_EXEC_RSP_DECODE_H__
#define SHELL_EXEC_RSP_DECODE_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "zcbor_decode.h"
#include "shell_exec_rsp_types.h"

#if DEFAULT_MAX_QTY != 12
#error "The type file was generated with a different default_max_qty than this file"
#endif


int cbor_decode_shell_exec_rsp(
		const uint8_t *payload, size_t payload_len,
		struct shell_exec_rsp *result,
		size_t *payload_len_out);


#endif /* SHELL_EXEC_RSP_DECODE_H__ */
