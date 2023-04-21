#
# Copyright (c) 2018-2021 mcumgr authors
#
# SPDX-License-Identifier: Apache-2.0
#

target_include_directories(MCUMGR INTERFACE
    include
)

target_include_directories(MCUMGR INTERFACE
    zcbor/include
)

zephyr_library_sources(
    src/zephyr_shell_mgmt.c
    src/shell_mgmt.c
)

if(CONFIG_MCUMGR_CLIENT_SHELL_MGMT)
zephyr_library_sources(
	zcbor/source/shell_exec_cmd_encode.c
	zcbor/source/shell_exec_rsp_decode.c
	src/shell_mgmt_client.c
)
endif()