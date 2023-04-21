/*
 * Generated using zcbor version 0.6.0
 * https://github.com/NordicSemiconductor/zcbor
 * Generated with a --default-max-qty of 12
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "zcbor_decode.h"
#include "shell_exec_cmd_decode.h"

#if DEFAULT_MAX_QTY != 12
#error "The type file was generated with a different default_max_qty than this file"
#endif

static bool decode_shell_exec_cmd(zcbor_state_t *state, struct shell_exec_cmd *result);


static bool decode_shell_exec_cmd(
		zcbor_state_t *state, struct shell_exec_cmd *result)
{
	zcbor_print("%s\r\n", __func__);
	struct zcbor_string tmp_str;

	bool tmp_result = (((zcbor_map_start_decode(state) && (((((zcbor_tstr_expect(state, ((tmp_str.value = (uint8_t *)"argv", tmp_str.len = sizeof("argv") - 1, &tmp_str)))))
	&& (zcbor_list_start_decode(state) && ((zcbor_multi_decode(0, 12, &(*result).args_count, (zcbor_decoder_t *)zcbor_tstr_decode, state, (&(*result).args), sizeof(struct zcbor_string))) || (zcbor_list_map_end_force_decode(state), false)) && zcbor_list_end_decode(state)))) || (zcbor_list_map_end_force_decode(state), false)) && zcbor_map_end_decode(state))));

	if (!tmp_result)
		zcbor_trace();

	return tmp_result;
}



int cbor_decode_shell_exec_cmd(
		const uint8_t *payload, size_t payload_len,
		struct shell_exec_cmd *result,
		size_t *payload_len_out)
{
	zcbor_state_t states[4];

	zcbor_new_state(states, sizeof(states) / sizeof(zcbor_state_t), payload, payload_len, 1);

	bool ret = decode_shell_exec_cmd(states, result);

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
