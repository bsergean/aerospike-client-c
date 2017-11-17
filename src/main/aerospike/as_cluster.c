/*
 * Copyright 2008-2017 Aerospike, Inc.
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
#include <aerospike/as_cluster.h>
#include <aerospike/as_address.h>
#include <aerospike/as_admin.h>
#include <aerospike/as_info.h>
#include <aerospike/as_log_macros.h>
#include <aerospike/as_lookup.h>
#include <aerospike/as_password.h>
#include <aerospike/as_peers.h>
#include <aerospike/as_shm_cluster.h>
#include <aerospike/as_socket.h>
#include <aerospike/as_string.h>
#include <aerospike/as_tls.h>
#include <aerospike/as_vector.h>

#include <citrusleaf/alloc.h>
#include <citrusleaf/cf_byte_order.h>
#include <citrusleaf/cf_clock.h>

/******************************************************************************
 * Globals
 *****************************************************************************/

extern uint32_t as_event_loop_capacity;
uint32_t as_cluster_count = 0;

/******************************************************************************
 * Function declarations
 *****************************************************************************/

as_status
as_node_refresh(as_cluster* cluster, as_error* err, as_node* node, as_peers* peers);

as_status
as_node_refresh_peers(as_cluster* cluster, as_error* err, as_node* node, as_peers* peers);

as_status
as_node_refresh_partitions(as_cluster* cluster, as_error* err, as_node* node, as_peers* peers);

/******************************************************************************
 * Functions
 *****************************************************************************/

static inline void
set_nodes(as_cluster* cluster, as_nodes* nodes)
{
	as_fence_store();
	as_store_ptr(&cluster->nodes, nodes);
}

static as_nodes*
as_nodes_create(uint32_t capacity)
{
	size_t size = sizeof(as_nodes) + (sizeof(as_node*) * capacity);
	as_nodes* nodes = cf_malloc(size);
	memset(nodes, 0, size);
	nodes->ref_count = 1;
	nodes->size = capacity;
	return nodes;
}

/**
 * Use non-inline function for garbarge collector function pointer reference.
 * Forward to inlined release.
 */
static void
release_nodes(as_nodes* nodes)
{
	as_nodes_release(nodes);
}

static void
as_cluster_event_notify(as_cluster* cluster, as_node* node, as_cluster_event_type type)
{
	if (! cluster->event_callback) {
		return;
	}

	if (node != NULL) {
		as_cluster_event event = {
			.node_name = node->name,
			.node_address = as_node_get_address_string(node),
			.udata = cluster->event_callback_udata,
			.type = type
		};
		cluster->event_callback(&event);
	}
	else {
		as_cluster_event event = {
			.node_name = "",
			.node_address = "",
			.udata = cluster->event_callback_udata,
			.type = type
		};
		cluster->event_callback(&event);
	}
}

/**
 * Add nodes using copy on write semantics.
 */
void
as_cluster_add_nodes_copy(as_cluster* cluster, as_vector* /* <as_node*> */ nodes_to_add)
{
	// Log node additions.
	as_node* node;
	for (uint32_t i = 0; i < nodes_to_add->size; i++) {
		node = as_vector_get_ptr(nodes_to_add, i);
		as_log_info("Add node %s %s", node->name, as_node_get_address_string(node));
		as_cluster_event_notify(cluster, node, AS_CLUSTER_ADD_NODE);
	}
	
	// Create temporary nodes array.
	as_nodes* nodes_old = cluster->nodes;
	as_nodes* nodes_new = as_nodes_create(nodes_old->size + nodes_to_add->size);

	// Add existing nodes.
	memcpy(nodes_new->array, nodes_old->array, sizeof(as_node*) * nodes_old->size);
		
	// Add new nodes.
	memcpy(&nodes_new->array[nodes_old->size], nodes_to_add->list, sizeof(as_node*) * nodes_to_add->size);
		
	// Replace nodes with copy.
	set_nodes(cluster, nodes_new);
	
	// Put old nodes on garbage collector stack.
	as_gc_item item;
	item.data = nodes_old;
	item.release_fn = (as_release_fn)release_nodes;
	as_vector_append(cluster->gc, &item);
}

static void
as_cluster_add_nodes(as_cluster* cluster, as_vector* /* <as_node*> */ nodes_to_add)
{
	as_cluster_add_nodes_copy(cluster, nodes_to_add);

	// Update shared memory nodes.
	if (cluster->shm_info) {
		as_shm_add_nodes(cluster, nodes_to_add);
	}
}

const char*
as_cluster_get_alternate_host(as_cluster* cluster, const char* hostname)
{
	// Return alternate IP address if specified in ip_map.
	// Useful when there are both internal and external addresses for the same node.
	const char* alt = hostname;

	if (cluster->ip_map) {
		as_vector* ip_map = cluster->ip_map;
		
		for (uint32_t i = 0; i < ip_map->size; i++) {
			as_addr_map* entry = as_vector_get(ip_map, i);
			if (strcmp(entry->orig, hostname) == 0) {
				// Found mapping for this address.  Use alternate.
				as_log_debug("Using %s instead of %s", entry->alt, hostname);
				alt = entry->alt;
				break;
			}
		}
	}
	return alt;
}

static as_status
as_cluster_seed_nodes(as_cluster* cluster, as_error* err, bool enable_warnings)
{
	// Add all nodes at once to avoid copying entire array multiple times.
	as_vector nodes_to_add;
	as_vector_inita(&nodes_to_add, sizeof(as_node*), 64);

	as_node_info node_info;
	as_error error_local;
	as_error_init(&error_local); // AEROSPIKE_ERR_TIMEOUT doesn't come with a message; make sure it's initialized.
	as_status status = AEROSPIKE_OK;
	
	pthread_mutex_lock(&cluster->seed_lock);
	as_vector* seeds = cluster->seeds;

	for (uint32_t i = 0; i < seeds->size; i++) {
		as_host* seed = as_vector_get(seeds, i);
		
		const char* hostname = as_cluster_get_alternate_host(cluster, seed->name);
		as_address_iterator iter;
		as_status status = as_lookup_host(&iter, &error_local, hostname, seed->port);
		
		if (status != AEROSPIKE_OK) {
			if (enable_warnings) {
				as_log_warn("Failed to lookup %s %d. %s %s", hostname, seed->port, as_error_string(status), error_local.message);
			}
			continue;
		}
		
		struct sockaddr* addr;

		while (as_lookup_next(&iter, &addr)) {
			status = as_lookup_node(cluster, &error_local, seed->tls_name, addr, &node_info);
			
			if (status == AEROSPIKE_OK) {
				as_node* node = as_peers_find_local_node(&nodes_to_add, node_info.name);
				
				if (node) {
					as_socket_close(&node_info.socket);
					as_node_add_address(node, addr);
					
					if (iter.hostname_is_alias) {
						as_node_add_alias(node, hostname, seed->port);
					}
				}
				else {
					node = as_node_create(cluster, hostname, seed->tls_name, seed->port, iter.hostname_is_alias, addr, &node_info);
					as_vector_append(&nodes_to_add, &node);
				}
			}
			else {
				if (enable_warnings) {
					as_log_warn("Failed to connect to seed %s %d. %s %s", hostname, seed->port, as_error_string(status), error_local.message);
				}
			}
		}
		as_lookup_end(&iter);
	}
	pthread_mutex_unlock(&cluster->seed_lock);

	if (nodes_to_add.size > 0) {
		as_cluster_add_nodes(cluster, &nodes_to_add);
		status = AEROSPIKE_OK;
	}
	else {
		status = as_error_set_message(err, AEROSPIKE_ERR_CLIENT, "Failed to connect");
	}
	
	as_vector_destroy(&nodes_to_add);
	return status;
}

static bool
as_cluster_find_node_in_map(as_cluster* cluster, as_node* node)
{
	if (cluster->shm_info) {
		return as_shm_partition_tables_find_node(cluster->shm_info->cluster_shm, node);
	}
	return as_partition_tables_find_node(cluster->partition_tables, node);
}

static void
as_cluster_find_nodes_to_remove(as_cluster* cluster, uint32_t refresh_count, as_vector* /* <as_node*> */ nodes_to_remove)
{
	as_nodes* nodes = cluster->nodes;
	
	for (uint32_t i = 0; i < nodes->size; i++) {
		as_node* node = nodes->array[i];
		
		if (! node->active) {
			// Inactive nodes must be removed.
			as_vector_append(nodes_to_remove, &node);
			continue;
		}

		if (refresh_count == 0 && node->failures >= 5) {
			// All node info requests failed and this node had 5 consecutive failures.
			// Remove node.  If no nodes are left, seeds will be tried in next cluster
			// tend iteration.
			as_vector_append(nodes_to_remove, &node);
			continue;
		}

		if (nodes->size > 1 && refresh_count >= 1 && node->friends == 0) {
			// Node is not referenced by other nodes.
			// Check if node responded to info request.
			if (node->failures == 0) {
				// Node is alive, but not referenced by other nodes.  Check if mapped.
				if (! as_cluster_find_node_in_map(cluster, node)) {
					// Node doesn't have any partitions mapped to it.
					// There is no point in keeping it in the cluster.
					as_vector_append(nodes_to_remove, &node);
				}
			}
			else {
				// Node not responding. Remove it.
				as_vector_append(nodes_to_remove, &node);
			}
		}
	}
}

static bool
as_cluster_find_node_by_reference(as_vector* /* <as_node*> */ nodes_to_remove, as_node* filter)
{
	as_node* node;
	
	for (uint32_t i = 0; i < nodes_to_remove->size; i++) {
		node = as_vector_get_ptr(nodes_to_remove, i);
		
		// Duplicate nodes can exist because single node clusters may be reseeded.  Then, a seeded
		// node with the same name can exist with the unresponsive node.  Therefore, check pointer
		// equality only and not name.
		if (node == filter) {
			return true;
		}
	}
	return false;
}

/**
 * Use non-inline function for garbarge collector function pointer reference.
 * Forward to inlined release.
 */
static void
release_node(as_node* node)
{
	as_node_release(node);
}

/**
 * Remove nodes using copy on write semantics.
 */
void
as_cluster_remove_nodes_copy(as_cluster* cluster, as_vector* /* <as_node*> */ nodes_to_remove)
{
	// Create temporary nodes array.
	// Since nodes are only marked for deletion using node references in the nodes array,
	// and the tend thread is the only thread modifying nodes, we are guaranteed that nodes
	// in nodes_to_remove exist.  Therefore, we know the final array size.
	as_nodes* nodes_old = cluster->nodes;
	as_nodes* nodes_new = as_nodes_create(nodes_old->size - nodes_to_remove->size);
	as_node* node;
	uint32_t count = 0;
		
	// Add nodes that are not in remove list.
	for (uint32_t i = 0; i < nodes_old->size; i++) {
		node = nodes_old->array[i];
		
		if (as_cluster_find_node_by_reference(nodes_to_remove, node)) {
			as_log_info("Remove node %s %s", node->name, as_node_get_address_string(node));
			as_cluster_event_notify(cluster, node, AS_CLUSTER_REMOVE_NODE);
			as_gc_item item;
			item.data = node;
			item.release_fn = (as_release_fn)release_node;
			as_vector_append(cluster->gc, &item);
		}
		else {
			if (count < nodes_new->size) {
				nodes_new->array[count++] = node;
			}
			else {
				as_log_error("Remove node error. Node count exceeded %u, %s %s", count, node->name, as_node_get_address_string(node));
			}
		}
	}
		
	// Do sanity check to make sure assumptions are correct.
	if (count < nodes_new->size) {
		as_log_warn("Node remove mismatch. Expected %u Received %u", nodes_new->size, count);
	}

	// Replace nodes with copy.
	set_nodes(cluster, nodes_new);

	if (nodes_new->size == 0) {
		as_cluster_event_notify(cluster, NULL, AS_CLUSTER_DISCONNECTED);
	}

	// Put old nodes on garbage collector stack.
	as_gc_item item;
	item.data = nodes_old;
	item.release_fn = (as_release_fn)release_nodes;
	as_vector_append(cluster->gc, &item);
}

static void
as_cluster_remove_nodes(as_cluster* cluster, as_vector* /* <as_node*> */ nodes_to_remove)
{
	// There is no need to delete nodes from partition tables because the nodes
	// have already been set to inactive. Further connection requests will result
	// in an exception and a different node will be tried.
	
	// Set node to inactive.
	for (uint32_t i = 0; i < nodes_to_remove->size; i++) {
		as_node* node = as_vector_get_ptr(nodes_to_remove, i);
		as_node_deactivate(node);
	}
			
	// Remove all nodes at once to avoid copying entire array multiple times.
	as_cluster_remove_nodes_copy(cluster, nodes_to_remove);
	
	// Update shared memory nodes.
	if (cluster->shm_info) {
		as_shm_remove_nodes(cluster, nodes_to_remove);
	}
}

static as_status
as_cluster_set_partition_size(as_cluster* cluster, as_error* err)
{
	as_nodes* nodes = cluster->nodes;
	as_status status = AEROSPIKE_OK;
	
	for (uint32_t i = 0; i < nodes->size && cluster->n_partitions == 0; i++) {
		as_node* node = nodes->array[i];

		char* response = 0;
		uint64_t deadline = as_socket_deadline(cluster->conn_timeout_ms);
		status = as_info_command_node(err, node, "partitions", true, deadline, &response);

		if (status != AEROSPIKE_OK) {
			continue;
		}

		char *value = 0;
		status = as_info_parse_single_response(response, &value);
			
		if (status == AEROSPIKE_OK) {
			cluster->n_partitions = atoi(value);
		}
		else {
			as_error_update(err, status, "Invalid partitions info response from node %s: %s", node->name, response);
		}
		cf_free(response);
	}
	
	if (cluster->n_partitions > 0) {
		// Must reset error if previous nodes had failed.
		if (status != AEROSPIKE_OK) {
			as_error_reset(err);
		}
		return AEROSPIKE_OK;
	}
	
	// Return error code if no nodes are currently in cluster.
	if (status == AEROSPIKE_OK) {
		return as_error_update(err, AEROSPIKE_ERR_CLIENT, "Failed to retrieve partition size from empty cluster");
	}
	return status;
}

/**
 * Release data structures schuleduled for removal in previous cluster tend.
 */
static void
as_cluster_gc(as_vector* /* <as_gc_item> */ vector)
{
	for (uint32_t i = 0; i < vector->size; i++) {
		as_gc_item* item = as_vector_get(vector, i);
		item->release_fn(item->data);
	}
	as_vector_clear(vector);
}

/**
 * Check health of all nodes in the cluster.
 */
as_status
as_cluster_tend(as_cluster* cluster, as_error* err, bool enable_seed_warnings)
{
	// All node additions/deletions are performed in tend thread.
	// Garbage collect data structures released in previous tend.
	// This tend interval delay substantially reduces the chance of
	// deleting a ref counted data structure when other threads
	// are stuck between assignment and incrementing the ref count.
	as_cluster_gc(cluster->gc);

	// If active nodes don't exist, seed cluster.
	as_nodes* nodes = cluster->nodes;

	if (nodes->size == 0) {
		as_status status = as_cluster_seed_nodes(cluster, err, enable_seed_warnings);
		
		if (status != AEROSPIKE_OK) {
			return status;
		}
	}

	// Retrieve fixed number of partitions only once from any node.
	if (cluster->n_partitions == 0) {
		as_status status = as_cluster_set_partition_size(cluster, err);
		
		if (status != AEROSPIKE_OK) {
			return status;
		}
	}
	
	// Initialize tend iteration node statistics.
	as_peers peers;
	as_vector_inita(&peers.hosts, sizeof(as_host), 16);
	as_vector_inita(&peers.nodes, sizeof(as_node*), 16);
	peers.use_peers = true;
	peers.gen_changed = false;
	
	nodes = cluster->nodes;
	for (uint32_t i = 0; i < nodes->size; i++) {
		as_node* node = nodes->array[i];
		node->friends = 0;
		node->partition_changed = false;
		
		if (! (node->features & AS_FEATURES_PEERS)) {
			peers.use_peers = false;
		}
	}
	
	// Refresh all known nodes.
	as_error error_local;
	uint32_t refresh_count = 0;
	
	for (uint32_t i = 0; i < nodes->size; i++) {
		as_node* node = nodes->array[i];
		
		if (node->active) {
			as_status status = as_node_refresh(cluster, &error_local, node, &peers);
			
			if (status == AEROSPIKE_OK) {
				node->failures = 0;
				refresh_count++;
			}
			else {
				// Use info level so aql doesn't see message by default.
				as_log_info("Node %s refresh failed: %s %s", node->name, as_error_string(status), error_local.message);
				if (peers.use_peers) {
					peers.gen_changed = true;
				}
				node->failures++;
			}
		}
	}
	
	// Refresh peers when necessary.
	if (peers.gen_changed) {
		// Refresh peers for all nodes that responded the first time even if only one node's peers changed.
		refresh_count = 0;

		for (uint32_t i = 0; i < nodes->size; i++) {
			as_node* node = nodes->array[i];
			
			if (node->failures == 0 && node->active) {
				as_status status = as_node_refresh_peers(cluster, &error_local, node, &peers);
				
				if (status == AEROSPIKE_OK) {
					refresh_count++;
				}
				else {
					as_log_warn("Node %s peers refresh failed: %s %s", node->name, as_error_string(status), error_local.message);
					node->failures++;
				}
			}
		}
	}
	
	// Refresh partition map when necessary.
	for (uint32_t i = 0; i < nodes->size; i++) {
		as_node* node = nodes->array[i];
		
		// Avoid "split cluster" case where this node thinks it's a 1-node cluster.
		// Unchecked, such a node can dominate the partition map and cause all other
		// nodes to be dropped.
		if (node->partition_changed && node->failures == 0 && node->active && (node->peers_count > 0 || refresh_count == 1)) {
			as_status status = as_node_refresh_partitions(cluster, &error_local, node, &peers);
			
			if (status != AEROSPIKE_OK) {
				as_log_warn("Node %s partition refresh failed: %s %s", node->name, as_error_string(status), error_local.message);
				node->failures++;
			}
		}
	}

	if (peers.gen_changed || ! peers.use_peers) {
		// Handle nodes changes determined from refreshes.
		as_vector nodes_to_remove;
		as_vector_inita(&nodes_to_remove, sizeof(as_node*), nodes->size);

		as_cluster_find_nodes_to_remove(cluster, refresh_count, &nodes_to_remove);
		
		// Remove nodes in a batch.
		if (nodes_to_remove.size > 0) {
			as_cluster_remove_nodes(cluster, &nodes_to_remove);
		}
		as_vector_destroy(&nodes_to_remove);
	}
	
	// Add nodes in a batch.
	if (peers.nodes.size > 0) {
		as_cluster_add_nodes(cluster, &peers.nodes);
	}
	
	as_vector* hosts = &peers.hosts;
	
	for (uint32_t i = 0; i < hosts->size; i++) {
		as_host* host = as_vector_get(hosts, i);
		as_host_destroy(host);
	}
	as_vector_destroy(hosts);
	as_vector_destroy(&peers.nodes);
	return AEROSPIKE_OK;
}

/**
 * Tend the cluster until it has stabilized and return control.
 * This helps avoid initial database request timeout issues when
 * a large number of threads are initiated at client startup.
 *
 * At least two cluster tends are necessary. The first cluster
 * tend finds a seed node and obtains the seed's partition maps 
 * and peer nodes.  The second cluster tend requests partition 
 * maps from the peer nodes.
 *
 * A third cluster tend is allowed if some peers nodes can't
 * be contacted.  If peer nodes are still unreachable, an
 * error is returned.
 */
static as_status
as_wait_till_stabilized(as_cluster* cluster, as_error* err)
{
	uint32_t count = -1;

	for (int i = 0; i < 3; i++) {
		as_status status = as_cluster_tend(cluster, err, true);

		if (status != AEROSPIKE_OK) {
			return status;
		}

		// Check to see if cluster has changed since the last tend.
		// If not, assume cluster has stabilized and return.
		as_nodes* nodes = cluster->nodes;

		if (count == nodes->size) {
			return AEROSPIKE_OK;
		}
		count = nodes->size;
	}
	return as_error_set_message(err, AEROSPIKE_ERR_CLIENT, "Cluster not stabilized after multiple tend attempts");
}

static void*
as_cluster_tender(void* data)
{
	as_cluster* cluster = (as_cluster*)data;
	
	struct timespec delta;
	cf_clock_set_timespec_ms(cluster->tend_interval, &delta);
	
	struct timespec abstime;
	
	as_status status;
	as_error err;
	
	pthread_mutex_lock(&cluster->tend_lock);

	while (cluster->valid) {
		status = as_cluster_tend(cluster, &err, false);
		
		if (status != AEROSPIKE_OK) {
			as_log_warn("Tend error: %s %s", as_error_string(status), err.message);
		}
		
		// Convert tend interval into absolute timeout.
		cf_clock_current_add(&delta, &abstime);
		
		// Sleep for tend interval and exit early if cluster destroy is signaled.
		pthread_cond_timedwait(&cluster->tend_cond, &cluster->tend_lock, &abstime);
	}
	pthread_mutex_unlock(&cluster->tend_lock);

	as_tls_thread_cleanup();
	
	return NULL;
}

static int
as_cluster_find_seed(as_vector* seeds, const char* hostname, uint16_t port) {
	for (uint32_t i = 0; i < seeds->size; i++) {
		as_host* seed = as_vector_get(seeds, i);

		if (seed->port == port && strcmp(seed->name, hostname) == 0) {
			return (int)i;
		}
	}
	return -1;
}

void
as_cluster_add_seed(as_cluster* cluster, const char* hostname, const char* tls_name, uint16_t port)
{
	pthread_mutex_lock(&cluster->seed_lock);
	as_vector* seeds = cluster->seeds;

	if (as_cluster_find_seed(seeds, hostname, port) < 0) {
		as_host* seed = as_vector_reserve(seeds);
		as_host_copy_fields(seed, hostname, tls_name, port);
		as_log_debug("Add seed %s %d", seed->name, seed->port);
	}
	pthread_mutex_unlock(&cluster->seed_lock);
}

void
as_cluster_remove_seed(as_cluster* cluster, const char* hostname, uint16_t port)
{
	pthread_mutex_lock(&cluster->seed_lock);
	as_vector* seeds = cluster->seeds;

	// Remove all seeds even if there are duplicates.
	int index;

	do {
		index = as_cluster_find_seed(seeds, hostname, port);

		if (index >= 0) {
			as_host* seed = as_vector_get(seeds, index);
			as_host_destroy(seed);
			as_vector_remove(seeds, index);
			as_log_debug("Remove seed %s %d", hostname, port);
		}
	} while (index >= 0);

	pthread_mutex_unlock(&cluster->seed_lock);
}

static void
as_cluster_add_seed_address(as_cluster* cluster, as_node* node, as_address* address)
{
	struct sockaddr* addr = (struct sockaddr*)&address->addr;
	char address_name[AS_IP_ADDRESS_SIZE];

	as_address_short_name(addr, address_name, sizeof(address_name));
	uint16_t port = as_address_port(addr);

	as_cluster_add_seed(cluster, address_name, node->tls_name, port);
}

void
as_cluster_add_seeds(as_cluster* cluster)
{
	// Add other nodes as seeds, if they don't already exist.
	as_nodes* nodes = cluster->nodes;

	for (uint32_t i = 0; i < nodes->size; i++) {
		as_node* node = nodes->array[i];
		as_address* addresses = node->addresses;

		for (uint32_t j = 0; j < node->address4_size; j++) {
			as_cluster_add_seed_address(cluster, node, &addresses[j]);
		}
		
		uint32_t max = AS_ADDRESS4_MAX + node->address6_size;
		
		for (uint32_t j = AS_ADDRESS4_MAX; j < max; j++) {
			as_cluster_add_seed_address(cluster, node, &addresses[j]);
		}
	}
}

as_status
as_cluster_init(as_cluster* cluster, as_error* err, bool fail_if_not_connected)
{
	// Tend cluster until all nodes identified.
	as_status status = as_wait_till_stabilized(cluster, err);
	
	if (status != AEROSPIKE_OK) {
		if (fail_if_not_connected) {
			return status;
		}
		else {
			as_log_warn(err->message);
			as_error_reset(err);
		}
	}
	as_cluster_add_seeds(cluster);
	cluster->valid = true;
	return AEROSPIKE_OK;
}

as_node*
as_node_get_random(as_cluster* cluster)
{
	as_nodes* nodes = as_nodes_reserve(cluster);
	uint32_t size = nodes->size;

	for (uint32_t i = 0; i < size; i++) {
		// Must handle concurrency with other threads.
		uint32_t index = as_faa_uint32(&cluster->node_index, 1);
		as_node* node = nodes->array[index % size];
		uint8_t active = as_load_uint8(&node->active);

		if (active) {
			as_node_reserve(node);
			as_nodes_release(nodes);
			return node;
		}
	}
	as_nodes_release(nodes);
	return NULL;
}

as_node*
as_node_get_by_name(as_cluster* cluster, const char* name)
{
	as_nodes* nodes = as_nodes_reserve(cluster);
	
	for (uint32_t i = 0; i < nodes->size; i++) {
		as_node* node = nodes->array[i];
		
		if (strcmp(node->name, name) == 0) {
			as_node_reserve(node);
			as_nodes_release(nodes);
			return node;
		}
	}
	as_nodes_release(nodes);
	return(0);
}

void
as_cluster_get_node_names(as_cluster* cluster, int* n_nodes, char** node_names)
{
	as_nodes* nodes = as_nodes_reserve(cluster);
	uint32_t size = nodes->size;
	*n_nodes = size;
	
	if (size == 0) {
		*node_names = 0;
		as_nodes_release(nodes);
		return;
	}
	
	*node_names = cf_malloc(AS_NODE_NAME_SIZE * size);
	if (*node_names == 0) {
		as_nodes_release(nodes);
		return;
	}
	
	char* nptr = *node_names;
	for (uint32_t i = 0; i < size; i++) {
		as_node* node = nodes->array[i];
		memcpy(nptr, node->name, AS_NODE_NAME_SIZE);
		nptr += AS_NODE_NAME_SIZE;
	}
	as_nodes_release(nodes);
}

as_status
as_cluster_get_node(
	as_cluster* cluster, as_error* err, const char* ns, const uint8_t* digest,
	as_policy_replica replica, bool master, as_node** node_pp
	)
{
#ifdef AS_TEST_PROXY
	as_node* node = as_node_get_random(cluster);
#else
	if (cluster->shm_info) {
		return as_shm_cluster_get_node(cluster, err, ns, digest, replica, master, node_pp);
	}

	as_partition_table* table = as_cluster_get_partition_table(cluster, ns);

	if (! table) {
		*node_pp = NULL;
		return as_error_update(err, AEROSPIKE_ERR_CLIENT, "Invalid namespace: %s", ns);
	}

	uint32_t partition_id = as_partition_getid(digest, cluster->n_partitions);
	as_partition* p = &table->partitions[partition_id];
	as_node* node = as_partition_get_node(cluster, p, replica, master, table->cp_mode);
#endif

	if (! node) {
		*node_pp = NULL;
		return as_error_update(err, AEROSPIKE_ERR_CLIENT, "Invalid node for key.");
	}

	*node_pp = node;
	return AEROSPIKE_OK;
}

bool
as_cluster_is_connected(as_cluster* cluster)
{
	if (! cluster) {
		return false;
	}
	
	as_nodes* nodes = as_nodes_reserve(cluster);
	bool connected = false;
	
	if (nodes->size > 0 && cluster->valid) {
		// Even though nodes exist, they may not be currently responding.  Check further.
		for (uint32_t i = 0; i < nodes->size; i++) {
			as_node* node = nodes->array[i];

			// Mark connected if any node is active and cluster tend consecutive info request 
			// failures are less than 5.
			if (node->active && node->failures < 5) {
				connected = true;
				break;
			}
		}
	}
	as_nodes_release(nodes);
	return connected;
}

void
as_cluster_change_password(as_cluster* cluster, const char* user, const char* password)
{
	if (user && *user) {
		if (cluster->user) {
			if (strcmp(cluster->user, user) == 0) {
				cf_free(cluster->password);
				cluster->password = cf_strdup(password);
			}
		}
		else {
			cluster->user = cf_strdup(user);
			cf_free(cluster->password);
			cluster->password = cf_strdup(password);
		}
	}
}

as_status
as_cluster_create(as_config* config, as_error* err, as_cluster** cluster_out)
{
	as_incr_uint32(&as_cluster_count);
	
	as_cluster* cluster = cf_malloc(sizeof(as_cluster));
	memset(cluster, 0, sizeof(as_cluster));
	
	// Initialize user/password.
	if (*(config->user)) {
		cluster->user = cf_strdup(config->user);
	}
	
	if (*(config->password)) {
		cluster->password = cf_strdup(config->password);
	}
	
	// Heap allocated cluster_name continues to be owned by as->config.
	// Make a reference copy here.
	cluster->cluster_name = config->cluster_name;
	cluster->event_callback = config->event_callback;
	cluster->event_callback_udata = config->event_callback_udata;

	// Initialize cluster tend and node parameters
	cluster->tend_interval = (config->tender_interval < 250)? 250 : config->tender_interval;
	cluster->max_conns_per_node = config->max_conns_per_node;
	cluster->conn_timeout_ms = (config->conn_timeout_ms == 0) ? 1000 : config->conn_timeout_ms;
	cluster->max_socket_idle = (config->max_socket_idle > 86400) ? 86400 : config->max_socket_idle;
	cluster->async_max_conns_per_node = config->async_max_conns_per_node;
	cluster->pipe_max_conns_per_node = config->pipe_max_conns_per_node;;
	cluster->conn_pools_per_node = config->conn_pools_per_node;
	cluster->use_services_alternate = config->use_services_alternate;

	// Initialize seed hosts.  Round initial capacity up to multiple of 16.
	as_vector* src = config->hosts;
	as_vector* trg = as_vector_create(sizeof(as_host), (src->size + 15) & ~15);
	for (uint32_t i = 0; i < src->size; i++) {
		as_host* src_seed = as_vector_get(src, i);
		as_host* trg_seed = as_vector_reserve(trg);
		as_host_copy(src_seed, trg_seed);
	}
	cluster->seeds = trg;
	pthread_mutex_init(&cluster->seed_lock, NULL);

	// Initialize IP map translation if provided.
	if (config->ip_map && config->ip_map_size > 0) {
		cluster->ip_map = as_vector_create(sizeof(as_addr_map), config->ip_map_size);

		for (uint32_t i = 0; i < config->ip_map_size; i++) {
			as_addr_map* src_addr = &config->ip_map[i];
			as_addr_map* trg_addr = as_vector_reserve(cluster->ip_map);

			trg_addr->orig = cf_strdup(src_addr->orig);
			trg_addr->alt = cf_strdup(src_addr->alt);
		}
	}

	if (as_event_loop_capacity > 0) {
		// Create one pending integer for each event loop.
		cluster->pending = cf_calloc(as_event_loop_capacity, sizeof(int));
	}

	// Initialize tend lock and condition.
	pthread_mutex_init(&cluster->tend_lock, NULL);
	pthread_cond_init(&cluster->tend_cond, NULL);

	// Initialize empty nodes.
	cluster->nodes = as_nodes_create(0);
	
	// Initialize empty partition tables.
	cluster->partition_tables = as_partition_tables_create(0);
	
	// Initialize garbage collection array.
	cluster->gc = as_vector_create(sizeof(as_gc_item), 8);
	
	// Initialize thread pool.
	int rc = as_thread_pool_init(&cluster->thread_pool, config->thread_pool_size);

	// Setup per-thread TLS cleanup function.
	cluster->thread_pool.fini_fn = as_tls_thread_cleanup;
	
	if (rc) {
		as_status status = as_error_update(err, AEROSPIKE_ERR_CLIENT, "Failed to initialize thread pool of size %u: %d",
				config->thread_pool_size, rc);
		as_cluster_destroy(cluster);
		*cluster_out = 0;
		return status;
	}

	// Initialize TLS parameters.
	as_status st = as_tls_context_setup(&config->tls, &cluster->tls_ctx, err);
	if (st != AEROSPIKE_OK) {
		as_cluster_destroy(cluster);
		*cluster_out = 0;
		return st;
	}

	if (config->use_shm) {
		// Create shared memory cluster.
		as_status status = as_shm_create(cluster, err, config);
		
		if (status != AEROSPIKE_OK) {
			as_cluster_destroy(cluster);
			*cluster_out = 0;
			return status;
		}
	}
	else {
		// Initialize normal cluster.
		as_status status = as_cluster_init(cluster, err, config->fail_if_not_connected);
		
		if (status != AEROSPIKE_OK) {
			as_cluster_destroy(cluster);
			*cluster_out = 0;
			return status;
		}
		// Run cluster tend thread.
		pthread_create(&cluster->tend_thread, 0, as_cluster_tender, cluster);
	}
	*cluster_out = cluster;
	return AEROSPIKE_OK;
}

void
as_cluster_destroy(as_cluster* cluster)
{
	// Shutdown thread pool.
	int rc = as_thread_pool_destroy(&cluster->thread_pool);
	
	if (rc) {
		as_log_warn("Failed to destroy thread pool: %d", rc);
	}

	// Stop tend thread and wait till finished.
	if (cluster->valid) {
		cluster->valid = false;
		
		// Signal tend thread to wake up from sleep and stop.
		pthread_mutex_lock(&cluster->tend_lock);
		pthread_cond_signal(&cluster->tend_cond);
		pthread_mutex_unlock(&cluster->tend_lock);
		
		// Wait for tend thread to finish.
		pthread_join(cluster->tend_thread, NULL);
		
		if (cluster->shm_info) {
			as_shm_destroy(cluster);
		}
	}

	// Release everything in garbage collector.
	as_cluster_gc(cluster->gc);
	as_vector_destroy(cluster->gc);
		
	// Release partition tables.
	as_partition_tables* tables = cluster->partition_tables;
	for (uint32_t i = 0; i < tables->size; i++) {
		as_partition_table_destroy(tables->array[i]);
	}
	as_partition_tables_release(tables);
	
	// Release nodes.
	as_nodes* nodes = cluster->nodes;
	for (uint32_t i = 0; i < nodes->size; i++) {
		as_node_release(nodes->array[i]);
	}
	as_nodes_release(nodes);
	
	// Destroy IP map.
	if (cluster->ip_map) {
		as_vector* ip_map = cluster->ip_map;
		for (uint32_t i = 0; i < ip_map->size; i++) {
			as_addr_map* addr = as_vector_get(ip_map, i);
			cf_free(addr->orig);
			cf_free(addr->alt);
		}
		as_vector_destroy(ip_map);
	}

	// Destroy seeds.
	pthread_mutex_lock(&cluster->seed_lock);
	as_vector* seeds = cluster->seeds;
	for (uint32_t i = 0; i < seeds->size; i++) {
		as_host* seed = as_vector_get(seeds, i);
		as_host_destroy(seed);
	}
	as_vector_destroy(seeds);
	pthread_mutex_unlock(&cluster->seed_lock);
	pthread_mutex_destroy(&cluster->seed_lock);

	// Destroy tend lock and condition.
	pthread_mutex_destroy(&cluster->tend_lock);
	pthread_cond_destroy(&cluster->tend_cond);

	cf_free(cluster->pending);
	cf_free(cluster->user);
	cf_free(cluster->password);

	// Do not free cluster name because as->config owns it.
	// cf_free(cluster->cluster_name);

	as_tls_context_destroy(&cluster->tls_ctx);

#if defined(_MSC_VER)
	WSACleanup();
#endif

	// Destroy cluster.
	cf_free(cluster);
	as_decr_uint32(&as_cluster_count);
}
