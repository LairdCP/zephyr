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
#include "shell_exec_cmd_encode.h"

#if DEFAULT_MAX_QTY != 12
#error "The type file was generated with a different default_max_qty than this file"
#endif

static bool encode_shell_exec_cmd(zcbor_state_t *state, const struct shell_exec_cmd *input);


static bool encode_shell_exec_cmd(
		zcbor_state_t *state, const struct shell_exec_cmd *input)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = (((zcbor_map_start_encode(state, 1) && (((((zcbor_tstr_encode(state, ((tmp_str.value = (uint8_t *)"argv", tmp_str.len = sizeof("argv") - 1, &tmp_str)))))
	&& (zcbor_list_start_encode(state, 12) && ((zcbor_multi_encode_minmax(0, 12, &(*input).args_count, (zcbor_encoder_t *)zcbor_tstr_encode, state, (&(*input).args), sizeof(struct zcbor_string))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, 12)))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_map_end_encode(state, 1))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}



int cbor_encode_shell_exec_cmd(
		uint8_t *payload, size_t payload_len,
		const struct shell_exec_cmd *input,
		size_t *payload_len_out)
{
	zcbor_state_t states[4];

	zcbor_new_state(states, sizeof(states) / sizeof(zcbor_state_t), payload, payload_len, 1);

	bool ret = encode_shell_exec_cmd(states, input);

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
