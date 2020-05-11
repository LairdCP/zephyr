/**
 * @file ipso_generic_sensor.c
 * @brief
 *
 * Copyright (c) 2017 Linaro Limited
 * Copyright (c) 2018-2019 Foundries.io
 * Copyright (c) 2020 Laird Connectivity
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <logging/log.h>
#define LOG_MODULE_NAME ipso_generic_sensor
#define LOG_LEVEL CONFIG_LWM2M_LOG_LEVEL
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

/******************************************************************************/
/* Includes                                                                   */
/******************************************************************************/
#include <stdint.h>
#include <init.h>

#include "lwm2m_object.h"
#include "lwm2m_engine.h"
#include "lwm2m_resource_ids.h"

/******************************************************************************/
/* Local Constant, Macro and Type Definitions                                 */
/******************************************************************************/
#define NUMBER_OF_FIELDS 7
#define UNIT_STR_MAX_SIZE 8

/* clang-format off */
#define MAX_INSTANCE_COUNT  CONFIG_LWM2M_IPSO_GENERIC_SENSOR_INSTANCE_COUNT
#define IPSO_OBJECT_ID      CONFIG_LWM2M_IPSO_GENERIC_SENSOR_IPSO_OBJECT_ID
#define SENSOR_NAME         CONFIG_LWM2M_IPSO_GENERIC_SENSOR_NAME
/* clang-format on */

/******************************************************************************/
/* Local Data Definitions                                                     */
/******************************************************************************/
/* resource state variables */
static float32_value_t sensor_value[MAX_INSTANCE_COUNT];
static float32_value_t min_measured_value[MAX_INSTANCE_COUNT];
static float32_value_t max_measured_value[MAX_INSTANCE_COUNT];
static float32_value_t min_range_value[MAX_INSTANCE_COUNT];
static float32_value_t max_range_value[MAX_INSTANCE_COUNT];
static char units[MAX_INSTANCE_COUNT][UNIT_STR_MAX_SIZE];

static struct lwm2m_engine_obj generic_sensor;
static struct lwm2m_engine_obj_field fields[NUMBER_OF_FIELDS] = {
	OBJ_FIELD_DATA(SENSOR_VALUE_RID, R, FLOAT32),
	OBJ_FIELD_DATA(MIN_MEASURED_VALUE_RID, R_OPT, FLOAT32),
	OBJ_FIELD_DATA(MAX_MEASURED_VALUE_RID, R_OPT, FLOAT32),
	OBJ_FIELD_DATA(MIN_RANGE_VALUE_RID, R_OPT, FLOAT32),
	OBJ_FIELD_DATA(MAX_RANGE_VALUE_RID, R_OPT, FLOAT32),
	OBJ_FIELD_EXECUTE_OPT(RESET_MIN_MAX_MEASURED_VALUES_RID),
	OBJ_FIELD_DATA(UNITS_RID, R_OPT, STRING),
};

static struct lwm2m_engine_obj_inst inst[MAX_INSTANCE_COUNT];
static struct lwm2m_engine_res_inst res[MAX_INSTANCE_COUNT][NUMBER_OF_FIELDS];

/******************************************************************************/
/* Local Function Definitions                                                 */
/******************************************************************************/
static void update_min_measured(u16_t obj_inst_id, int index)
{
	min_measured_value[index].val1 = sensor_value[index].val1;
	min_measured_value[index].val2 = sensor_value[index].val2;
	NOTIFY_OBSERVER(IPSO_OBJECT_ID, obj_inst_id, MIN_MEASURED_VALUE_RID);
}

static void update_max_measured(u16_t obj_inst_id, int index)
{
	max_measured_value[index].val1 = sensor_value[index].val1;
	max_measured_value[index].val2 = sensor_value[index].val2;
	NOTIFY_OBSERVER(IPSO_OBJECT_ID, obj_inst_id, MAX_MEASURED_VALUE_RID);
}

static int reset_min_max_measured_values_cb(u16_t obj_inst_id)
{
	int i;

	LOG_DBG("RESET MIN/MAX %d", obj_inst_id);
	for (i = 0; i < MAX_INSTANCE_COUNT; i++) {
		if (inst[i].obj && inst[i].obj_inst_id == obj_inst_id) {
			update_min_measured(obj_inst_id, i);
			update_max_measured(obj_inst_id, i);
			return 0;
		}
	}

	return -ENOENT;
}

static int sensor_value_write_cb(u16_t obj_inst_id, u8_t *data, u16_t data_len,
				 bool last_block, size_t total_size)
{
	int i;
	bool update_min = false;
	bool update_max = false;

	for (i = 0; i < MAX_INSTANCE_COUNT; i++) {
		if (inst[i].obj && inst[i].obj_inst_id == obj_inst_id) {
			/* update min / max */
			if (sensor_value[i].val1 < min_measured_value[i].val1) {
				update_min = true;
			} else if (sensor_value[i].val1 ==
					   min_measured_value[i].val1 &&
				   sensor_value[i].val2 <
					   min_measured_value[i].val2) {
				update_min = true;
			}

			if (sensor_value[i].val1 > max_measured_value[i].val1) {
				update_max = true;
			} else if (sensor_value[i].val1 ==
					   max_measured_value[i].val1 &&
				   sensor_value[i].val2 >
					   max_measured_value[i].val2) {
				update_max = true;
			}

			if (update_min) {
				update_min_measured(obj_inst_id, i);
			}

			if (update_max) {
				update_max_measured(obj_inst_id, i);
			}
		}
	}

	return 0;
}

static struct lwm2m_engine_obj_inst *generic_sensor_create(u16_t obj_inst_id)
{
	int index, i = 0;

	/* Check that there is no other instances with this ID */
	for (index = 0; index < MAX_INSTANCE_COUNT; index++) {
		if (inst[index].obj && inst[index].obj_inst_id == obj_inst_id) {
			LOG_ERR("Can not create instance - "
				"already existing: %u",
				obj_inst_id);
			return NULL;
		}
	}

	for (index = 0; index < MAX_INSTANCE_COUNT; index++) {
		if (!inst[index].obj) {
			break;
		}
	}

	if (index >= MAX_INSTANCE_COUNT) {
		LOG_ERR("Can not create instance - no more room: %u",
			obj_inst_id);
		return NULL;
	}

	/* Set default values */
	sensor_value[index].val1 = 0;
	sensor_value[index].val2 = 0;
	units[index][0] = '\0';
	min_measured_value[index].val1 = INT32_MAX;
	min_measured_value[index].val2 = 0;
	max_measured_value[index].val1 = INT32_MIN;
	max_measured_value[index].val2 = 0;
	min_range_value[index].val1 = 0;
	min_range_value[index].val2 = 0;
	max_range_value[index].val1 = 0;
	max_range_value[index].val2 = 0;

	/* initialize instance resource data */
	INIT_OBJ_RES(res[index], i, SENSOR_VALUE_RID, 0, &sensor_value[index],
		     sizeof(*sensor_value), NULL, NULL, sensor_value_write_cb,
		     NULL);
	INIT_OBJ_RES_DATA(res[index], i, UNITS_RID, units[index],
			  UNIT_STR_MAX_SIZE);
	INIT_OBJ_RES_DATA(res[index], i, MIN_MEASURED_VALUE_RID,
			  &min_measured_value[index],
			  sizeof(*min_measured_value));
	INIT_OBJ_RES_DATA(res[index], i, MAX_MEASURED_VALUE_RID,
			  &max_measured_value[index],
			  sizeof(*max_measured_value));
	INIT_OBJ_RES_DATA(res[index], i, MIN_RANGE_VALUE_RID,
			  &min_range_value[index], sizeof(*min_range_value));
	INIT_OBJ_RES_DATA(res[index], i, MAX_RANGE_VALUE_RID,
			  &max_range_value[index], sizeof(*max_range_value));
	INIT_OBJ_RES_EXECUTE(res[index], i, RESET_MIN_MAX_MEASURED_VALUES_RID,
			     reset_min_max_measured_values_cb);

	inst[index].resources = res[index];
	inst[index].resource_count = i;
	LOG_DBG("Create IPSO %s Sensor instance: %d", SENSOR_NAME, obj_inst_id);
	return &inst[index];
}

static int ipso_generic_sensor_init(struct device *dev)
{
	/* Set default values */
	(void)memset(inst, 0, sizeof(*inst) * MAX_INSTANCE_COUNT);
	(void)memset(res, 0,
		     sizeof(struct lwm2m_engine_res_inst) * MAX_INSTANCE_COUNT *
			     NUMBER_OF_FIELDS);

	generic_sensor.obj_id = IPSO_OBJECT_ID;
	generic_sensor.fields = fields;
	generic_sensor.field_count = ARRAY_SIZE(fields);
	generic_sensor.max_instance_count = MAX_INSTANCE_COUNT;
	generic_sensor.create_cb = generic_sensor_create;
	lwm2m_register_obj(&generic_sensor);

	return 0;
}

/******************************************************************************/
/* Global Function Definitions                                                */
/******************************************************************************/
SYS_INIT(ipso_generic_sensor_init, APPLICATION,
	 CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
