/*
 * Copyright 2008-2016 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy of
 * the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 */
#pragma once

#include <aerospike/as_cluster.h>

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************
 *	TYPES
 *****************************************************************************/

/**
 *	@private
 *	Name value pair.
 */
typedef struct as_name_value_s {
	char* name;
	char* value;
} as_name_value;

struct sockaddr_in;

/******************************************************************************
 * FUNCTIONS
 ******************************************************************************/

/**
 *	@private
 *	Send info command to specific host. The values must be freed by the caller on success.
 */
as_status
as_info_command_host(as_cluster* cluster, as_error* err, struct sockaddr_in* sa_in, char* command,
	 bool send_asis, uint64_t deadline_ms, char** response);

/**
 *	@private
 *	Return the single command's info response buffer value.
 *	The original buffer will be modified with the null termination character.
 */
as_status
as_info_parse_single_response(char *values, char **value);

/**
 *	@private
 *	Parse info response buffer into name/value pairs, one for each command.
 *	The original buffer will be modified with null termination characters to
 *	delimit each command name and value referenced by the name/value pairs.
 */
void
as_info_parse_multi_response(char* buf, as_vector* /* <as_name_value> */ values);

#ifdef __cplusplus
} // end extern "C"
#endif
