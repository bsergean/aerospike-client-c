/*
 * Copyright 2008-2015 Aerospike, Inc.
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
#include <aerospike/as_event.h>
#include <aerospike/as_event_internal.h>
#include <aerospike/as_admin.h>
#include <aerospike/as_command.h>
#include <aerospike/as_log_macros.h>
#include <aerospike/as_pipe.h>
#include <aerospike/as_proto.h>
#include <citrusleaf/alloc.h>
#include <errno.h>

/******************************************************************************
 * GLOBALS
 *****************************************************************************/

as_event_loop* as_event_loops = 0;
uint32_t as_event_loop_capacity = 0;
uint32_t as_event_loop_size = 0;
uint32_t as_event_loop_current = 0;
int as_event_send_buffer_size = 0;
int as_event_recv_buffer_size = 0;
bool as_event_threads_created = false;

/******************************************************************************
 * PUBLIC FUNCTIONS
 *****************************************************************************/

as_event_loop*
as_event_create_loops(uint32_t capacity)
{
	as_event_send_buffer_size = as_pipe_get_send_buffer_size();
	as_event_recv_buffer_size = as_pipe_get_recv_buffer_size();
	
	as_event_loops = cf_malloc(sizeof(as_event_loop) * capacity);
	
	if (! as_event_loops) {
		return 0;
	}
	
	as_event_loop_capacity = capacity;
	as_event_threads_created = true;
	
	for (uint32_t i = 0; i < capacity; i++) {
		as_event_loop* event_loop = &as_event_loops[i];
		event_loop->loop = 0;
		pthread_mutex_init(&event_loop->lock, 0);
		event_loop->thread = 0;
		event_loop->index = i;
		event_loop->initialized = false;
		
		if (! as_event_create_loop(event_loop)) {
			as_event_close_loops();
			return 0;
		}
		as_event_loop_size++;
	}
	return as_event_loops;
}

bool
as_event_set_external_loop_capacity(uint32_t capacity)
{
	as_event_send_buffer_size = as_pipe_get_send_buffer_size();
	as_event_recv_buffer_size = as_pipe_get_recv_buffer_size();
	
	size_t mem_size = sizeof(as_event_loop) * capacity;
	as_event_loops = cf_malloc(mem_size);
	
	if (! as_event_loops) {
		return false;
	}
	
	memset(as_event_loops, 0, mem_size);
	as_event_loop_capacity = capacity;
	as_event_threads_created = false;
	return true;
}

as_event_loop*
as_event_set_external_loop(void* loop)
{
	uint32_t current = ck_pr_faa_32(&as_event_loop_size, 1);
	
	if (current >= as_event_loop_capacity) {
		as_log_error("Failed to add external loop. Capacity is %u", as_event_loop_capacity);
		return 0;
	}
	
	as_event_loop* event_loop = &as_event_loops[current];
	event_loop->loop = loop;
	pthread_mutex_init(&event_loop->lock, 0);
	event_loop->thread = pthread_self();  // Current thread must be same as event loop thread!
	event_loop->index = current;
	event_loop->initialized = false;
	as_event_register_external_loop(event_loop);
	return event_loop;
}

void
as_event_close_loops()
{
	if (! as_event_loops) {
		return;
	}
	
	bool join = true;
	
	for (uint32_t i = 0; i < as_event_loop_size; i++) {
		as_event_loop* event_loop = &as_event_loops[i];
		
		// Send stop signal to loop.
		if (! as_event_close_loop(event_loop)) {
			as_log_error("Failed to send stop command to event loop");
			join = false;
		}
	}
	
	if (as_event_threads_created && join) {
		// Join threads.
		for (uint32_t i = 0; i < as_event_loop_size; i++) {
			as_event_loop* event_loop = &as_event_loops[i];
			pthread_join(event_loop->thread, NULL);
		}
	}
	cf_free(as_event_loops);
	as_event_loops = NULL;
}

/******************************************************************************
 * PRIVATE FUNCTIONS
 *****************************************************************************/

void
as_event_command_execute(as_event_command* cmd)
{
	ck_pr_inc_32(&cmd->node->async_pending);
	
	// Use pointer comparison for performance.
	// If portability becomes an issue, use "pthread_equal(event_loop->thread, pthread_self())"
	// instead.
	if (cmd->event_loop->thread == pthread_self()) {
		// We are already in event loop thread, so start processing.
		as_event_command_begin(cmd);
	}
	else {
		if (cmd->timeout_ms) {
			// Store current time in first 8 bytes which is not used yet.
			*(uint64_t*)cmd = cf_getms();
		}
		
		// Send command through queue so it can be executed in event loop thread.
		if (! as_event_send(cmd)) {
			as_error err;
			as_error_set_message(&err, AEROSPIKE_ERR_CLIENT, "Failed to queue command");
			as_event_error_callback(cmd, &err);
		}
	}
}

static inline void
as_event_command_free_worker(as_event_command* cmd)
{
	if (cmd->free_buf) {
		cf_free(cmd->buf);
	}
	cf_free(cmd);
}

#if defined AS_USE_LIBUV
static void
as_event_command_free_callback(uv_handle_t* handle)
{
	as_event_command_free_worker(handle->data);
}

static inline void
as_event_command_free(as_event_command* cmd)
{
	if (cmd->timeout_ms == 0) {
		as_event_command_free_worker(cmd);
	}
	else {
		uv_close((uv_handle_t*)&cmd->timer, as_event_command_free_callback);
	}
}
#else
#define as_event_command_free(x) as_event_command_free_worker(x)
#endif

static inline void
as_event_put_connection(as_event_command* cmd)
{
	as_queue* q = &cmd->node->async_conn_qs[cmd->event_loop->index];
	
	if (! as_queue_push_limit(q, &cmd->conn)) {
		as_event_close_connection(cmd->conn, cmd->node);
	}
}

static inline void
as_event_response_complete(as_event_command* cmd)
{
	if (cmd->pipeline) {
		as_pipe_response_complete(cmd);
		return;
	}
	
	as_event_stop_timer(cmd);
	as_event_stop_watcher(cmd, cmd->conn);
	as_event_put_connection(cmd);
	ck_pr_dec_32(&cmd->node->async_pending);
	as_node_release(cmd->node);
}

void
as_event_executor_complete(as_event_command* cmd)
{
	as_event_response_complete(cmd);
	
	// Only invoke user callback after all node commands have completed.
	as_event_executor* executor = cmd->udata;
	
	if (++executor->count == executor->max) {
		// All commands completed.
		if (executor->valid) {
			executor->complete_fn(executor, 0);
		}
		cf_free(executor->commands);
		cf_free(executor);
	}
	else {
		// Determine if a new command needs to be started.
		if (executor->valid) {
			int next = executor->count + executor->max_concurrent - 1;
			
			if (next < executor->max) {
				// Start new command.
				as_event_command_execute(executor->commands[next]);
			}
		}
	}
	as_event_command_free(cmd);
}

bool
as_event_get_connection(as_event_command* cmd)
{
	as_queue* q = &cmd->node->async_conn_qs[cmd->event_loop->index];
	as_async_connection* conn;

	// Find connection.
	while (as_queue_pop(q, &conn)) {
		if (as_event_validate_connection(&conn->base, false)) {
			conn->cmd = cmd;
			cmd->conn = (as_event_connection*)conn;
			return true;
		}
		as_event_close_connection(&conn->base, cmd->node);
	}
	
	// Create connection.
	conn = cf_malloc(sizeof(as_async_connection));
	conn->base.pipeline = false;
	conn->cmd = cmd;
	cmd->conn = &conn->base;
	return false;
}

void
as_event_error_callback(as_event_command* cmd, as_error* err)
{
	ck_pr_dec_32(&cmd->node->async_pending);
	as_node_release(cmd->node);
	
	switch (cmd->type) {
		case AS_ASYNC_TYPE_WRITE:
			((as_async_write_command*)cmd)->listener(err, cmd->udata, cmd->event_loop);
			break;
		case AS_ASYNC_TYPE_RECORD:
			((as_async_record_command*)cmd)->listener(err, 0, cmd->udata, cmd->event_loop);
			break;
		case AS_ASYNC_TYPE_VALUE:
			((as_async_value_command*)cmd)->listener(err, 0, cmd->udata, cmd->event_loop);
			break;
			
		default: {
			// Handle command that is part of a group (batch, scan, query).
			// Commands are issued on same event loop, so we can assume single threaded behavior.
			as_event_executor* executor = cmd->udata;
			
			// Notify user of error only once.
			if (executor->valid) {
				executor->complete_fn(executor, err);
				executor->valid = false;
			}
			
			// Only free executor if all outstanding commands are complete.
			if (++executor->count == executor->max) {
				cf_free(executor->commands);
				cf_free(executor);
			}
		}
	}
	as_event_command_free(cmd);
}

void
as_event_connect_error(as_event_command* cmd, as_error* err)
{
	// Only timer needs to be released on socket connection failure.
	// Watcher has not been registered yet.
	as_event_stop_timer(cmd);

	// Connection never opened, but memory still needs to be released.
	as_event_close_connection(cmd->conn, NULL);
	as_event_error_callback(cmd, err);
}

void
as_event_socket_error(as_event_command* cmd, as_error* err)
{
	if (cmd->pipeline) {
		as_pipe_socket_error(cmd, err);
		return;
	}
	
	// Socket read/write failure.
	as_event_stop_watcher(cmd, cmd->conn);
	
	// Stop timer.
	as_event_stop_timer(cmd);
	
	// Do not put connection back in pool.
	as_event_close_connection(cmd->conn, cmd->node);
	as_event_error_callback(cmd, err);
}

void
as_event_response_error(as_event_command* cmd, as_error* err)
{
	if (cmd->pipeline) {
		as_pipe_response_error(cmd, err);
		return;
	}
	
	// Server sent back error.
	// Release resources, make callback and free command.
	as_event_stop_timer(cmd);
	as_event_stop_watcher(cmd, cmd->conn);
	
	// Close socket on errors that can leave unread data in socket.
	switch (err->code) {
		case AEROSPIKE_ERR_QUERY_ABORTED:
		case AEROSPIKE_ERR_SCAN_ABORTED:
		case AEROSPIKE_ERR_ASYNC_CONNECTION:
		case AEROSPIKE_ERR_CLIENT_ABORT:
		case AEROSPIKE_ERR_CLIENT:
			as_event_close_connection(cmd->conn, cmd->node);
			break;
			
		default:
			as_event_put_connection(cmd);
			break;
	}
	as_event_error_callback(cmd, err);
}

void
as_event_timeout(as_event_command* cmd)
{
	if (cmd->pipeline) {
		as_pipe_timeout(cmd);
		return;
	}
	
	as_error err;
	as_error_set_message(&err, AEROSPIKE_ERR_TIMEOUT, as_error_string(AEROSPIKE_ERR_TIMEOUT));
	
	// Command has timed out.
	// Stop watcher if it has been initialized.
	if (cmd->state > AS_ASYNC_STATE_UNREGISTERED) {
		as_event_stop_watcher(cmd, cmd->conn);
	}
	
	// Assume timer has already been stopped.
	// Do not put connection back in pool.
	as_event_close_connection(cmd->conn, cmd->node);
	as_event_error_callback(cmd, &err);
}

bool
as_event_command_parse_header(as_event_command* cmd)
{
	as_msg* msg = (as_msg*)cmd->buf;
	
	if (msg->result_code == AEROSPIKE_OK) {
		as_event_response_complete(cmd);
		((as_async_write_command*)cmd)->listener(0, cmd->udata, cmd->event_loop);
		as_event_command_free(cmd);
	}
	else {
		as_error err;
		as_error_set_message(&err, msg->result_code, as_error_string(msg->result_code));
		as_event_response_error(cmd, &err);
	}
	return true;
}

bool
as_event_command_parse_result(as_event_command* cmd)
{
	as_msg* msg = (as_msg*)cmd->buf;
	as_msg_swap_header_from_be(msg);
	uint8_t* p = cmd->buf + sizeof(as_msg);
	as_status status = msg->result_code;
	
	switch (status) {
		case AEROSPIKE_OK: {
			as_record rec;
			
			if (msg->n_ops < 1000) {
				as_record_inita(&rec, msg->n_ops);
			}
			else {
				as_record_init(&rec, msg->n_ops);
			}
			
			rec.gen = msg->generation;
			rec.ttl = cf_server_void_time_to_ttl(msg->record_ttl);
			
			p = as_command_ignore_fields(p, msg->n_fields);
			as_command_parse_bins(&rec, p, msg->n_ops, cmd->deserialize);
			
			as_event_response_complete(cmd);
			((as_async_record_command*)cmd)->listener(0, &rec, cmd->udata, cmd->event_loop);
			as_event_command_free(cmd);
			as_record_destroy(&rec);
			break;
		}
			
		case AEROSPIKE_ERR_UDF: {
			as_error err;
			as_command_parse_udf_failure(p, &err, msg, status);
			as_event_response_error(cmd, &err);
			break;
		}
			
		default: {
			as_error err;
			as_error_set_message(&err, status, as_error_string(status));
			as_event_response_error(cmd, &err);
			break;
		}
	}
	return true;
}

bool
as_event_command_parse_success_failure(as_event_command* cmd)
{
	as_msg* msg = (as_msg*)cmd->buf;
	as_msg_swap_header_from_be(msg);
	uint8_t* p = cmd->buf + sizeof(as_msg);
	as_status status = msg->result_code;
	
	switch (status) {
		case AEROSPIKE_OK: {
			as_error err;
			as_val* val = 0;
			status = as_command_parse_success_failure_bins(&p, &err, msg, &val);
			
			if (status == AEROSPIKE_OK) {
				as_event_response_complete(cmd);
				((as_async_value_command*)cmd)->listener(0, val, cmd->udata, cmd->event_loop);
				as_event_command_free(cmd);
				as_val_destroy(val);
			}
			else {
				as_event_response_error(cmd, &err);
			}
			break;
		}
			
		case AEROSPIKE_ERR_UDF: {
			as_error err;
			as_command_parse_udf_failure(p, &err, msg, status);
			as_event_response_error(cmd, &err);
			break;
		}
			
		default: {
			as_error err;
			as_error_set_message(&err, status, as_error_string(status));
			as_event_response_error(cmd, &err);
			break;
		}
	}
	return true;
}
