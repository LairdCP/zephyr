/*
 * Generated using zcbor version 0.6.0
 * https://github.com/NordicSemiconductor/zcbor
 * Generated with a --default-max-qty of 12
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "zcbor_encode.h"
#include "shell_exec_rsp_encode.h"

#if DEFAULT_MAX_QTY != 12
#error "The type file was generated with a different default_max_qty than this file"
#endif

static bool encode_shell_exec_rsp(zcbor_state_t *state, const struct shell_exec_rsp *input);


static bool encode_shell_exec_rsp(
		zcbor_state_t *state, const struct shell_exec_rsp *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = (((zcbor_map_start_encode(state, 2) && (((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"o", tmp_str.len = sizeof("o") - 1, &tmp_str)))))
	&& (zcbor_tstr_encode(state, (&(*input).o))))
	&& (((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"rc", tmp_str.len = sizeof("rc") - 1, &tmp_str)))))
	&& (zcbor_int32_encode(state, (&(*input).rc))))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_map_end_encode(state, 2))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}



int cbor_encode_shell_exec_rsp(
		uint8_t *payload, size_t payload_len,
		const struct shell_exec_rsp *input,
		size_t *payload_len_out)
{
	zcbor_state_t states[3];

	zcbor_new_state(states, sizeof(states) / sizeof(zcbor_state_t), payload, payload_len, 1);

	bool ret = encode_shell_exec_rsp(states, input);

	if (ret && (payload_len_out != NULL)) {
		*payload_len_out = MIN(payload_len,
				(size_t)states[0].payload - (size_t)payload);
	}

	if (!ret) {
		int err = zcbor_pop_error(states);

		zcbor_print("Return error: %d\r\n", err);
		return (err == ZCBOR_SUCCESS) ? ZCBOR_ERR_UNKNOWN : err;
	}
	return ZCBOR_SUCCESS;
}
