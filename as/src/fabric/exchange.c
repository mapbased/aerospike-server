/*
 * exchange.c
 *
 * Copyright (C) 2016 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 */

#include "fabric/exchange.h"

#include <errno.h>
#include <sys/param.h> // For MAX() and MIN().

#include "citrusleaf/alloc.h"
#include "citrusleaf/cf_atomic.h"
#include "citrusleaf/cf_clock.h"
#include "citrusleaf/cf_queue.h"
#include "citrusleaf/cf_shash.h"

#include "dynbuf.h"
#include "fault.h"
#include "socket.h"

#include "base/cfg.h"
#include "base/datamodel.h"
#include "base/stats.h"
#include "fabric/fabric.h"
#include "fabric/hb.h"
#include "fabric/partition_balance.h"
#include "fabric/paxos.h"

/*
 * Overview
 * ========
 * Cluster data exchange state machine. Exchanges per namespace partition
 * version exchange for now, after evey cluster change.
 *
 * State transition diagram
 * ========================
 * The exchange state transition diagram responds to three events
 * 	1. Incoming message
 * 	2. Timer event
 * 	3. Clustering module's cluster change event.
 *
 * There are four states
 * 	1. Rest - the exchange is complete with all exchanged data committed.
 * 	2. Exchanging - the cluster has changed since the last commit and new data
 * exchange is in progress.
 * 	3. Ready to commit - this node has send its exchange data to all cluster
 * members, received corresponding acks and also exchange data from all cluster
 * members.
 * 	4. Orphaned - this node is an orphan. After a timeout blocks client
 * transactions.
 *
 * Exchange starts by being in the orphaned state.
 *
 * Code organization
 * =================
 *
 * There are different sections for each state. Each state has a dispatcher
 * which delegates the event handing to a state specific function. All state is
 * protected under a single lock.
 */

/*
 * ----------------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------------
 */

/**
 * Exchange protocol version information.
 */
#define AS_EXCHANGE_PROTOCOL_IDENTIFIER 1

/**
 * A soft limit for the maximum cluster size. Meant to be optimize hash and list
 * data structures and not as a limit on the number of nodes.
 */
#define AS_EXCHANGE_CLUSTER_MAX_SIZE_SOFT 200

/**
 * A soft limit for the maximum number of unique vinfo's in a namespace. Meant
 * to be optimize hash and list data structures and not as a limit on the number
 * of vinfos processed.
 */
#define AS_EXCHANGE_UNIQUE_VINFO_MAX_SIZE_SOFT 200

/**
 * Average number of partitions for a version information. Used as initial
 * allocation size for every unique vinfo, hence a smaller value.
 */
#define AS_EXCHANGE_VINFO_NUM_PIDS_AVG 1024

/**
 * Maximum event listeners.
 */
#define AS_EXTERNAL_EVENT_LISTENER_MAX 7

/**
 * Number of quantum intervals in orphan state after which client transactions
 * will be blocked.
 */
#define AS_EXCHANGE_TRANSACTION_BLOCK_ORPHAN_INTERVALS 5

/*
 * ----------------------------------------------------------------------------
 * Exchange data format for namespaces payload
 * ----------------------------------------------------------------------------
 */

/**
 * Partition data exchanged for each unique vinfo for a namespace.
 */
typedef struct as_exchange_vinfo_payload_s
{
	/**
	 * The partition vinfo.
	 */
	as_partition_version vinfo;

	/**
	 * Count of partitions having this vinfo.
	 */
	uint32_t num_pids;

	/**
	 * Partition having this vinfo.
	 */
	uint16_t pids[];
}__attribute__((__packed__)) as_exchange_vinfo_payload;

/**
 * Information exchanged for a single namespace.
 */
typedef struct as_exchange_namespace_payload_s
{
	/**
	 * Name of the namespace.
	 */
	char name[AS_ID_NAMESPACE_SZ];

	/**
	 * Count of version infos.
	 */
	uint32_t num_vinfos;

	/**
	 * Parition version information for each unique version.
	 */
	as_exchange_vinfo_payload vinfos[];
}__attribute__((__packed__)) as_exchange_namespace_payload;

/**
 * Information exchanged for all namespaces.
 */
typedef struct as_exchange_namespaces_payload_s
{
	/**
	 * The number of namespaces.
	 */
	uint32_t num_namespaces;

	/**
	 * Per namespace payload array.
	 */
	as_exchange_namespace_payload namespace_payloads[];
}__attribute__((__packed__)) as_exchange_namespaces_payload;

/**
 * Heap allocated incarnation of exchanged data for a single node.
 */
typedef struct as_exchange_node_data_s
{
	/**
	 * Exchanged data pointer.
	 */
	as_exchange_namespaces_payload *data;

	/**
	 * Size of exchanged data.
	 */
	size_t data_size;

	/**
	 * Allocated capacity for the exchanged data.
	 */
	size_t data_capacity;
} as_exchange_node_data;

/*
 * ----------------------------------------------------------------------------
 * Exchange internal data structures
 * ----------------------------------------------------------------------------
 */

/**
 * Exchange subsystem status.
 */
typedef enum
{
	AS_EXCHANGE_SYS_STATE_UNINITIALIZED,
	AS_EXCHANGE_SYS_STATE_RUNNING,
	AS_EXCHANGE_SYS_STATE_SHUTTING_DOWN,
	AS_EXCHANGE_SYS_STATE_STOPPED
} as_exchange_sys_state;

/**
 * Exchange message types.
 */
typedef enum
{
	/**
	 * Exchange data for one node.
	 */
	AS_EXCHANGE_MSG_TYPE_DATA,

	/**
	 * Ack on receipt of exchanged data.
	 */
	AS_EXCHANGE_MSG_TYPE_DATA_ACK,

	/**
	 * Not used.
	 */
	AS_EXCHANGE_MSG_TYPE_DATA_NACK,

	/**
	 * The source is ready to commit exchanged information.
	 */
	AS_EXCHANGE_MSG_TYPE_READY_TO_COMMIT,

	/**
	 * Message from the principal asking all nodes to commit the exchanged
	 * information.
	 */
	AS_EXCHANGE_MSG_TYPE_COMMIT,

	/**
	 * Sentinel value for exchange message types.
	 */
	AS_EXCHANGE_MSG_TYPE_SENTINEL
} as_exchange_msg_type;

/**
 * Internal exchange event type.
 */
typedef enum
{
	/**
	 * Cluster change event.
	 */
	AS_EXCHANGE_EVENT_CLUSTER_CHANGE,

	/**
	 * Timer event.
	 */
	AS_EXCHANGE_EVENT_TIMER,

	/**
	 * Incoming message event.
	 */
	AS_EXCHANGE_EVENT_MSG,
} as_exchange_event_type;

/**
 * Internal exchange event.
 */
typedef struct as_exchange_event_s
{
	/**
	 * The type of the event.
	 */
	as_exchange_event_type type;

	/**
	 * Message for incoming message events.
	 */
	msg* msg;

	/**
	 * Source for incoming message events.
	 */
	cf_node msg_source;

	/**
	 * Clustering event instance for clustering events.
	 */
	as_clustering_event* clustering_event;
} as_exchange_event;

/**
 * Exchange subsystem state in the state transition diagram.
 */
typedef enum as_exchange_state_s
{
	/**
	 * Exchange subsystem is at rest will all data exchanged synchronized and
	 * committed.
	 */
	AS_EXCHANGE_STATE_REST,

	/**
	 * Data exchange is in progress.
	 */
	AS_EXCHANGE_STATE_EXCHANGING,

	/**
	 * Data exchange is complete and this node is ready to commit data.
	 */
	AS_EXCHANGE_STATE_READY_TO_COMMIT,

	/**
	 * Self node is orphaned.
	 */
	AS_EXCHANGE_STATE_ORPHANED
} as_exchange_state;

/**
 * State for a single node in the succession list.
 */
typedef struct as_exchange_node_state_s
{
	/**
	 * Inidicates if peer node has acknowledged send from self.
	 */
	bool send_acked;

	/**
	 * Inidicates if self node has received data from this peer.
	 */
	bool received;

	/**
	 * Inidicates if this peer node is ready to commit. Only relevant and used
	 * by the current principal.
	 */
	bool is_ready_to_commit;

	/**
	 * Exchange data received from this peer node. Will be heap allocated and
	 * hence should be freed carefully while discarding this structure instance.
	 */
	as_exchange_node_data data;
} as_exchange_node_state;

/**
 * State maintained by the exchange subsystem.
 */
typedef struct as_exchange_s
{
	/**
	 * Exchange subsystem status.
	 */
	as_exchange_sys_state sys_state;

	/**
	 * Exchange state in the state transition diagram.
	 */
	as_exchange_state state;

	/**
	 * Time when this node's exchange data was sent out.
	 */
	cf_clock send_ts;

	/**
	 * Time when this node's ready to commit was sent out.
	 */
	cf_clock ready_to_commit_send_ts;

	/**
	 * Thread id of the timer event generator.
	 */
	pthread_t timer_tid;

	/**
	 * Nodes that are not yet ready to commit.
	 */
	cf_vector ready_to_commit_pending_nodes;

	/**
	 * Current cluster key.
	 */
	as_cluster_key cluster_key;

	/**
	 * Cluster size - size of the succession list.
	 */
	uint32_t cluster_size;

	/**
	 * Exchange's copy of the succession list.
	 */
	cf_vector succession_list;

	/**
	 * The principal node in current succession list. Always the first node.
	 */
	cf_node principal;

	/**
	 * Last committed cluster key.
	 */
	as_cluster_key committed_cluster_key;

	/**
	 * Last committed cluster size - size of the succession list.
	 */
	uint32_t committed_cluster_size;

	/**
	 * Last committed exchange's succession list.
	 */
	cf_vector committed_succession_list;

	/**
	 * The principal node in the committed succession list. Always the first
	 * node.
	 */
	cf_node committed_principal;

	/**
	 * The time this node entered orphan state.
	 */
	cf_clock orphan_state_start_time;

	/**
	 * Indicates if transactions have already been blocked in the orphan state.
	 */
	bool orphan_state_are_transactions_blocked;

	/**
	 * Will have an as_exchange_node_state entry for every node in the
	 * succession list.
	 */
	shash* nodeid_to_node_state;

	/**
	 * This node's data payload for current round.
	 */
	cf_dyn_buf self_data_dyn_buf;
} as_exchange;

/**
 * Internal storage for external event listeners.
 */
typedef struct as_exchange_event_listener_s
{
	/**
	 * The listener's calback function.
	 */
	as_exchange_cluster_changed_cb event_callback;

	/**
	 * The listeners user data object passed back as is to the callback
	 * function.
	 */
	void* udata;
} as_exchange_event_listener;

/**
 * External event publisher state.
 */
typedef struct as_exchange_external_event_publisher_s
{
	/**
	 * State of the external event publisher.
	 */
	as_exchange_sys_state sys_state;

	/**
	 * Inidicates if there is an event to publish.
	 */
	bool event_queued;

	/**
	 * The pending event to publish.
	 */
	as_exchange_cluster_changed_event to_publish;

	/**
	 * The static succession list published with the message.
	 */
	cf_vector published_succession_list;

	/**
	 * Conditional variable to signal a pending event.
	 */
	pthread_cond_t is_pending;

	/**
	 * Thread id of the publisher thread.
	 */
	pthread_t event_publisher_tid;

	/**
	 * Mutex to protect the conditional variable.
	 */
	pthread_mutex_t is_pending_mutex;

	/**
	 * External event listeners.
	 */
	as_exchange_event_listener event_listeners[AS_EXTERNAL_EVENT_LISTENER_MAX];

	/**
	 * Event listener count.
	 */
	uint event_listener_count;
} as_exchange_external_event_publisher;

/*
 * ----------------------------------------------------------------------------
 * Globals
 * ----------------------------------------------------------------------------
 */

/**
 * Singleton exchange state all initialized to zero.
 */
static as_exchange g_exchange = { 0 };

/**
 * The fields in the exchange message. Should never change the order or elements
 * in between.
 */
typedef enum
{
	AS_EXCHANGE_MSG_ID,
	AS_EXCHANGE_MSG_TYPE,
	AS_EXCHANGE_MSG_CLUSTER_KEY,
	AS_EXCHANGE_MSG_NAMESPACES_PAYLOAD
} as_exchange_msg_fields;

/**
 * Exchange message template.
 */
static msg_template g_exchange_msg_template[] = { {
	AS_EXCHANGE_MSG_ID,
	M_FT_UINT32 }, { AS_EXCHANGE_MSG_TYPE, M_FT_UINT32 }, {
	AS_EXCHANGE_MSG_CLUSTER_KEY,
	M_FT_UINT64 }, { AS_EXCHANGE_MSG_NAMESPACES_PAYLOAD, M_FT_BUF } };

/**
 * Global lock to serialize all reads and writes to the exchange state.
 */
pthread_mutex_t g_exchange_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

/**
 * Singleton external events publisher.
 */
static as_exchange_external_event_publisher g_external_event_publisher;

/**
 * The fat lock for all clustering events listener changes.
 */
static pthread_mutex_t g_external_event_publisher_lock =
		PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

/**
 * Acquire a lock on the event publisher.
 */
#define EXTERNAL_EVENT_PUBLISHER_LOCK()						\
({															\
	pthread_mutex_lock (&g_external_event_publisher_lock);	\
	LOCK_DEBUG("publisher locked in %s", __FUNCTION__);		\
})

/**
 * Relinquish the lock on the external event publisher.
 */
#define EXTERNAL_EVENT_PUBLISHER_UNLOCK()						\
({																\
	pthread_mutex_unlock (&g_external_event_publisher_lock);	\
	LOCK_DEBUG("publisher unLocked in %s", __FUNCTION__);		\
})

/*
 * ----------------------------------------------------------------------------
 * Logging macros.
 * ----------------------------------------------------------------------------
 */

/**
 * Used to limit potentially long log lines. Includes space for NULL terminator.
 */
#define LOG_LENGTH_MAX() (800)
#define CRASH(format, ...) cf_crash(AS_EXCHANGE, format, ##__VA_ARGS__)
#define WARNING(format, ...) cf_warning(AS_EXCHANGE, format, ##__VA_ARGS__)
#define INFO(format, ...) cf_info(AS_EXCHANGE, format, ##__VA_ARGS__)
#define DEBUG(format, ...) cf_debug(AS_EXCHANGE, format, ##__VA_ARGS__)
#define DETAIL(format, ...) cf_detail(AS_EXCHANGE, format, ##__VA_ARGS__)
#define LOG(severity, format, ...)			\
({											\
	switch (severity) {						\
	case CF_CRITICAL:						\
		CRASH(format, ##__VA_ARGS__);		\
		break;								\
	case CF_WARNING:						\
		WARNING(format, ##__VA_ARGS__);		\
		break;								\
	case CF_INFO:							\
		INFO(format, ##__VA_ARGS__);		\
		break;								\
	case CF_DEBUG:							\
		DEBUG(format, ##__VA_ARGS__);		\
		break;								\
	case CF_DETAIL:							\
		DETAIL(format, ##__VA_ARGS__);		\
		break;								\
	default:								\
		break;								\
	}										\
})

/**
 * Size of the self payload dynamic buffer.
 */
#define AS_EXCHANGE_SELF_DYN_BUF_SIZE() (AS_NAMESPACE_SZ * AS_EXCHANGE_UNIQUE_VINFO_MAX_SIZE_SOFT	\
		* ((AS_EXCHANGE_VINFO_NUM_PIDS_AVG * sizeof(uint16_t))										\
				+ sizeof(as_partition_version)))

/**
 * Scratch size for exchange messages.
 * TODO: Compute this properly.
 */
#define AS_EXCHANGE_MSG_SCRATCH_SIZE 2048

#ifdef LOCK_DEBUG_ENABLED
#define LOCK_DEBUG(format, ...) DEBUG(format, ##__VA_ARGS__)
#else
#define LOCK_DEBUG(format, ...)
#endif

/**
 * Acquire a lock on the exchange subsystem.
 */
#define EXCHANGE_LOCK()							\
({												\
	pthread_mutex_lock (&g_exchange_lock);		\
	LOCK_DEBUG("locked in %s", __FUNCTION__);	\
})

/**
 * Relinquish the lock on the exchange subsystem.
 */
#define EXCHANGE_UNLOCK()							\
({													\
	pthread_mutex_unlock (&g_exchange_lock);		\
	LOCK_DEBUG("unLocked in %s", __FUNCTION__);		\
})

/**
 * Timer event generation interval.
 */
#define EXCHANGE_TIMER_TICK_INTERVAL() (75)

/**
 * Minimum timeout interval for sent exchange data.
 */
#define EXCHANGE_SEND_MIN_TIMEOUT() (MAX(75, as_hb_tx_interval_get() / 2))

/**
 * Maximum timeout interval for sent exchange data.
 */
#define EXCHANGE_SEND_MAX_TIMEOUT() (30000)

/**
 * Timeout for receiving commit message after transitioning to ready to commit.
 */
#define EXCHANGE_READY_TO_COMMIT_TIMEOUT() (EXCHANGE_SEND_MIN_TIMEOUT())

/**
 * Send timeout is a step function with this value as the interval for each
 * step.
 */
#define EXCHANGE_SEND_STEP_INTERVAL()							\
(MAX(EXCHANGE_SEND_MIN_TIMEOUT(), as_hb_tx_interval_get()))

/**
 * Check if exchange is initialized.
 */
#define EXCHANGE_IS_INITIALIZED()						\
({														\
	EXCHANGE_LOCK();									\
	bool initialized = (g_exchange.sys_state			\
			!= AS_EXCHANGE_SYS_STATE_UNINITIALIZED);	\
	EXCHANGE_UNLOCK();									\
	initialized;										\
})

/**
 * * Check if exchange is running.
 */
#define EXCHANGE_IS_RUNNING()											\
({																		\
	EXCHANGE_LOCK();													\
	bool running = (EXCHANGE_IS_INITIALIZED()							\
			&& g_exchange.sys_state == AS_EXCHANGE_SYS_STATE_RUNNING);	\
	EXCHANGE_UNLOCK();													\
	running;															\
})

/**
 * Create temporary stack variables.
 */
#define TOKEN_PASTE(x, y) x##y
#define STACK_VAR(x, y) TOKEN_PASTE(x, y)

/**
 * Convert a vector to a stack allocated array.
 */
#define cf_vector_to_stack_array(vector_p, nodes_array_p, num_nodes_p)	\
({																		\
	*num_nodes_p = cf_vector_size(vector_p);							\
	if (*num_nodes_p > 0) {												\
		*nodes_array_p = alloca(sizeof(cf_node) * (*num_nodes_p));		\
		for (int i = 0; i < *num_nodes_p; i++) {						\
			cf_vector_get(vector_p, i, &(*nodes_array_p)[i]);			\
		}																\
	}																	\
	else {																\
		*nodes_array_p = NULL;											\
	}																	\
})

/**
 * Create and initialize a lockless stack allocated vector to initially sized to
 * store cluster node number of elements.
 */
#define cf_vector_stack_create(value_type)											\
({																					\
	cf_vector * STACK_VAR(vector, __LINE__) = (cf_vector*)alloca(					\
			sizeof(cf_vector));														\
	size_t buffer_size = AS_EXCHANGE_CLUSTER_MAX_SIZE_SOFT							\
			* sizeof(value_type);													\
	void* STACK_VAR(buff, __LINE__) = alloca(buffer_size); cf_vector_init_smalloc(	\
			STACK_VAR(vector, __LINE__), sizeof(value_type),						\
			(uint8_t*)STACK_VAR(buff, __LINE__), buffer_size,						\
			VECTOR_FLAG_INITZERO);													\
	STACK_VAR(vector, __LINE__);													\
})

/**
 * Put a key into a hash or crash with an error message on failure.
 */
#define SHASH_PUT_OR_DIE(hash, key, value, error, ...)							\
if (SHASH_OK != shash_put(hash, key, value)) {CRASH(error, ##__VA_ARGS__);}

/**
 * Delete a key from hash or on failure crash with an error message. Key not
 * found is NOT considered an error.
 */
#define SHASH_DELETE_OR_DIE(hash, key, error, ...)							\
if (SHASH_ERR == shash_delete(hash, key)) {CRASH(error, ##__VA_ARGS__);}

/**
 * Read value for a key and crash if there is an error. Key not found is NOT
 * considered an error.
 */
#define SHASH_GET_OR_DIE(hash, key, value, error, ...)	\
({														\
	int retval = shash_get(hash, key, value);			\
	if (retval == SHASH_ERR) {							\
		CRASH(error, ##__VA_ARGS__);					\
	}													\
	retval;												\
})

/*
 * ----------------------------------------------------------------------------
 * Vector functions to be moved to cf_vector
 * ----------------------------------------------------------------------------
 */

/**
 * Convert a vector to an array.
 * FIXME: return pointer to the internal vector storage.
 */
static cf_node*
vector_to_array(cf_vector* vector)
{
	return (cf_node*)vector->vector;
}

/**
 * Clear / delete all entries in a vector.
 */
static void
vector_clear(cf_vector* vector)
{
	cf_vector_delete_range(vector, 0, cf_vector_size(vector));
}

/**
 * Find the index of an element in the vector. Equality is based on mem compare.
 *
 * @param vector the source vector.
 * @param element the element to find.
 * @return the index if the element is found, -1 otherwise.
 */
static int
vector_find(cf_vector* vector, const void* element)
{
	int element_count = cf_vector_size(vector);
	size_t value_len = VECTOR_ELEM_SZ(vector);
	for (int i = 0; i < element_count; i++) {
		// No null check required since we are iterating under a lock and within
		// vector bounds.
		void* src_element = cf_vector_getp(vector, i);
		if (src_element) {
			if (memcmp(element, src_element, value_len) == 0) {
				return i;
			}
		}
	}
	return -1;
}

/**
 * Copy all elements form the source vector to the destination vector to the
 * destination vector. Assumes the source and destination vector are not being
 * modified while the copy operation is in progress.
 *
 * @param dest the destination vector.
 * @param src the source vector.
 * @return the number of elements copied.
 */
static int
vector_copy(cf_vector* dest, cf_vector* src)
{
	int element_count = cf_vector_size(src);
	int copied_count = 0;
	for (int i = 0; i < element_count; i++) {
		// No null check required since we are iterating under a lock and within
		// vector bounds.
		void* src_element = cf_vector_getp(src, i);
		if (src_element) {
			cf_vector_append(dest, src_element);
			copied_count++;
		}
	}
	return copied_count;
}

/**
 * Generate a hash code for a blob using Jenkins hash function.
 */
static uint32_t
exchange_blob_hash(const uint8_t* value, size_t value_size)
{
	uint32_t hash = 0;
	for (int i = 0; i < value_size; ++i) {
		hash += value[i];
		hash += (hash << 10);
		hash ^= (hash >> 6);
	}
	hash += (hash << 3);
	hash ^= (hash >> 11);
	hash += (hash << 15);

	return hash;
}

/**
 * Generate a hash code for a mesh node key.
 */
static uint32_t
exchange_vinfo_shash(const void* value)
{
	return exchange_blob_hash((const uint8_t*)value,
			sizeof(as_partition_version));
}

/*
 * ----------------------------------------------------------------------------
 * Clustering external event publisher
 * ----------------------------------------------------------------------------
 */

/**
 * * Check if event publisher is running.
 */
static bool
exchange_external_event_publisher_is_running()
{
	EXTERNAL_EVENT_PUBLISHER_LOCK();
	bool running = g_external_event_publisher.sys_state
			== AS_EXCHANGE_SYS_STATE_RUNNING;
	EXTERNAL_EVENT_PUBLISHER_UNLOCK();
	return running;
}

/**
 * Initialize the event publisher.
 */
static void
exchange_external_event_publisher_init()
{
	EXTERNAL_EVENT_PUBLISHER_LOCK();
	memset(&g_external_event_publisher, 0, sizeof(g_external_event_publisher));
	cf_vector_init(&g_external_event_publisher.published_succession_list,
			sizeof(cf_node),
			AS_EXCHANGE_CLUSTER_MAX_SIZE_SOFT, VECTOR_FLAG_INITZERO);

	pthread_mutex_init(&g_external_event_publisher.is_pending_mutex, NULL);
	pthread_cond_init(&g_external_event_publisher.is_pending, NULL);
	EXTERNAL_EVENT_PUBLISHER_UNLOCK();
}

/**
 * Register a clustering event listener.
 */
static void
exchange_external_event_listener_register(
		as_exchange_cluster_changed_cb event_callback, void* udata)
{
	EXTERNAL_EVENT_PUBLISHER_LOCK();

	if (g_external_event_publisher.event_listener_count
			>= AS_EXTERNAL_EVENT_LISTENER_MAX) {
		CRASH("cannot register more than %d event listeners",
				AS_EXTERNAL_EVENT_LISTENER_MAX);
	}

	g_external_event_publisher.event_listeners[g_external_event_publisher.event_listener_count].event_callback =
			event_callback;
	g_external_event_publisher.event_listeners[g_external_event_publisher.event_listener_count].udata =
			udata;
	g_external_event_publisher.event_listener_count++;

	EXTERNAL_EVENT_PUBLISHER_UNLOCK();
}

/**
 * Wakeup the publisher thread.
 */
static void
exchange_external_event_publisher_thr_wakeup()
{
	pthread_mutex_lock(&g_external_event_publisher.is_pending_mutex);
	pthread_cond_signal(&g_external_event_publisher.is_pending);
	pthread_mutex_unlock(&g_external_event_publisher.is_pending_mutex);
}

/**
 * Queue up and external event to publish.
 */
static void
exchange_external_event_queue(as_exchange_cluster_changed_event* event)
{
	EXTERNAL_EVENT_PUBLISHER_LOCK();
	memcpy(&g_external_event_publisher.to_publish, event,
			sizeof(g_external_event_publisher.to_publish));

	vector_clear(&g_external_event_publisher.published_succession_list);
	if (event->succession) {
		// Use the static list for the published event, so that the input event
		// object can be destroyed irrespective of when the it is published.
		for (int i = 0; i < event->cluster_size; i++) {
			cf_vector_append(
					&g_external_event_publisher.published_succession_list,
					&event->succession[i]);
		}
		g_external_event_publisher.to_publish.succession = vector_to_array(
				&g_external_event_publisher.published_succession_list);

	}
	else {
		g_external_event_publisher.to_publish.succession = NULL;
	}

	g_external_event_publisher.event_queued = true;

	EXTERNAL_EVENT_PUBLISHER_UNLOCK();

	// Wake up the publisher thread.
	exchange_external_event_publisher_thr_wakeup();
}

/**
 * Publish external events if any are pending.
 */
static void
exchange_external_events_publish()
{
	EXTERNAL_EVENT_PUBLISHER_LOCK();

	if (g_external_event_publisher.event_queued) {
		g_external_event_publisher.event_queued = false;
		for (uint i = 0; i < g_external_event_publisher.event_listener_count;
				i++) {
			(g_external_event_publisher.event_listeners[i].event_callback)(
					&g_external_event_publisher.to_publish,
					g_external_event_publisher.event_listeners[i].udata);
		}
	}
	EXTERNAL_EVENT_PUBLISHER_UNLOCK();
}

/**
 * External event publisher thread.
 */
static void*
exchange_external_event_publisher_thr(void* arg)
{
	pthread_mutex_lock(&g_external_event_publisher.is_pending_mutex);

	while (true) {
		pthread_cond_wait(&g_external_event_publisher.is_pending,
				&g_external_event_publisher.is_pending_mutex);
		if (exchange_external_event_publisher_is_running()) {
			exchange_external_events_publish();
		}
		else {
			// Publisher stopped, exit the tread.
			break;
		}
	}

	return NULL;
}

/**
 * Start the event publisher.
 */
static void
exchange_external_event_publisher_start()
{
	EXTERNAL_EVENT_PUBLISHER_LOCK();
	g_external_event_publisher.sys_state = AS_EXCHANGE_SYS_STATE_RUNNING;

	// Start the event publishing thread.
	if (pthread_create(&g_external_event_publisher.event_publisher_tid, 0,
			exchange_external_event_publisher_thr, NULL) != 0) {
		CRASH("could not create event publishing thread: %s",
				cf_strerror(errno));
	}
	EXTERNAL_EVENT_PUBLISHER_UNLOCK();
}

/**
 * Stop the event publisher.
 */
static void
external_event_publisher_stop()
{
	EXTERNAL_EVENT_PUBLISHER_LOCK();
	g_external_event_publisher.sys_state =
			AS_EXCHANGE_SYS_STATE_SHUTTING_DOWN;
	EXTERNAL_EVENT_PUBLISHER_UNLOCK();

	exchange_external_event_publisher_thr_wakeup();
	pthread_join(g_external_event_publisher.event_publisher_tid, NULL);

	EXTERNAL_EVENT_PUBLISHER_LOCK();
	g_external_event_publisher.sys_state = AS_EXCHANGE_SYS_STATE_STOPPED;
	g_external_event_publisher.event_queued = false;
	EXTERNAL_EVENT_PUBLISHER_UNLOCK();
}

/*
 * ----------------------------------------------------------------------------
 * Node state related
 * ----------------------------------------------------------------------------
 */

/**
 * Initialize node state.
 */
static void
exchange_node_state_init(as_exchange_node_state* node_state)
{
	memset(node_state, 0, sizeof(*node_state));
}

/**
 * Reset node state.
 */
static void
exchange_node_state_reset(as_exchange_node_state* node_state)
{
	node_state->send_acked = false;
	node_state->received = false;
	node_state->is_ready_to_commit = false;
	node_state->data.data_size = 0;
}

/**
 * Destroy node state.
 */
static void
exchange_node_state_destroy(as_exchange_node_state* node_state)
{
	if (node_state->data.data) {
		cf_free(node_state->data.data);
	}

	node_state->data.data = NULL;
	node_state->data.data_size = 0;
	node_state->data.data_capacity = 0;
}

/**
 * Reduce function to match node -> node state hash to the succession list.
 * Should always be invoked under a lock over the main hash.
 */
static int
exchange_node_states_reset_reduce(const void* key, void* data, void* udata)
{
	const cf_node* node = (const cf_node*)key;
	as_exchange_node_state* node_state = (as_exchange_node_state*)data;

	int node_index = vector_find(&g_exchange.succession_list, node);
	if (node_index < 0) {
		// Node not in succession list
		exchange_node_state_destroy(node_state);
		return SHASH_REDUCE_DELETE;
	}

	exchange_node_state_reset(node_state);
	return SHASH_OK;
}

/**
 * Adjust the nodeid_to_node_state hash to have an entry for every node in the
 * succession list with state reset for a new round of exchange. Removes entries
 * not in the succession list.
 */
static void
exchange_node_states_reset()
{
	EXCHANGE_LOCK();

	// Fix existing entries by reseting entries in succession and removing
	// entries not in succession list.
	shash_reduce_delete(g_exchange.nodeid_to_node_state,
			exchange_node_states_reset_reduce, NULL);

	// Add missing entries.
	int succession_length = cf_vector_size(&g_exchange.succession_list);

	as_exchange_node_state temp_state;
	for (int i = 0; i < succession_length; i++) {
		cf_node nodeid;

		cf_vector_get(&g_exchange.succession_list, i, &nodeid);
		if (shash_get(g_exchange.nodeid_to_node_state, &nodeid, &temp_state)
				== SHASH_ERR_NOTFOUND) {
			exchange_node_state_init(&temp_state);

			SHASH_PUT_OR_DIE(g_exchange.nodeid_to_node_state, &nodeid,
					&temp_state,
					"error creating new shash entry for node %"PRIx64, nodeid);
		}
	}

	EXCHANGE_UNLOCK();
}

/**
 * Reduce function to find nodes that had not acked self node's exchange data.
 */
static int
exchange_nodes_find_send_unacked_reduce(const void* key, void* data,
		void* udata)
{
	const cf_node* node = (const cf_node*)key;
	as_exchange_node_state* node_state = (as_exchange_node_state*)data;
	cf_vector* unacked = (cf_vector*)udata;

	if (!node_state->send_acked) {
		cf_vector_append(unacked, node);
	}
	return SHASH_OK;
}

/**
 * Find nodes that have not acked self node's exchange data.
 */
static void
exchange_nodes_find_send_unacked(cf_vector* unacked)
{
	shash_reduce_delete(g_exchange.nodeid_to_node_state,
			exchange_nodes_find_send_unacked_reduce, unacked);
}

/**
 * Reduce function to find peer nodes from whom self node has not received
 * exchange data.
 */
static int
exchange_nodes_find_not_received_reduce(const void* key, void* data,
		void* udata)
{
	const cf_node* node = (const cf_node*)key;
	as_exchange_node_state* node_state = (as_exchange_node_state*)data;
	cf_vector* not_received = (cf_vector*)udata;

	if (!node_state->received) {
		cf_vector_append(not_received, node);
	}
	return SHASH_OK;
}

/**
 * Find peer nodes from whom self node has not received exchange data.
 */
static void
exchange_nodes_find_not_received(cf_vector* not_received)
{
	shash_reduce_delete(g_exchange.nodeid_to_node_state,
			exchange_nodes_find_not_received_reduce, not_received);
}

/**
 * Reduce function to find peer nodes that are not ready to commit.
 */
static int
exchange_nodes_find_not_ready_to_commit_reduce(const void* key, void* data,
		void* udata)
{
	const cf_node* node = (const cf_node*)key;
	as_exchange_node_state* node_state = (as_exchange_node_state*)data;
	cf_vector* not_ready_to_commit = (cf_vector*)udata;

	if (!node_state->is_ready_to_commit) {
		cf_vector_append(not_ready_to_commit, node);
	}
	return SHASH_OK;
}

/**
 * Find peer nodes that are not ready to commit.
 */
static void
exchange_nodes_find_not_ready_to_commit(cf_vector* not_ready_to_commit)
{
	shash_reduce_delete(g_exchange.nodeid_to_node_state,
			exchange_nodes_find_not_ready_to_commit_reduce,
			not_ready_to_commit);
}

/**
 * Update the node state for a node.
 */
static void
exchange_node_state_update(cf_node nodeid, as_exchange_node_state* node_state)
{
	SHASH_PUT_OR_DIE(g_exchange.nodeid_to_node_state, &nodeid, node_state,
			"error updating node state from hash for node %"PRIx64, nodeid);
}

/**
 * Get state of a node from the hash. If not found crash because this entry
 * should be present in the hash.
 */
static void
exchange_node_state_get_safe(cf_node nodeid, as_exchange_node_state* node_state)
{
	if (SHASH_GET_OR_DIE(g_exchange.nodeid_to_node_state, &nodeid, node_state,
			"error reading node state from hash") == SHASH_ERR_NOTFOUND) {
		CRASH(
				"node entry for node %"PRIx64"  missing from node state hash", nodeid);
	}
}

/*
 * ----------------------------------------------------------------------------
 * Message related
 * ----------------------------------------------------------------------------
 */

/**
 * Fill compulsary fields in a message common to all message types.
 */
static void
exchange_msg_src_fill(msg* msg, as_exchange_msg_type type)
{
	EXCHANGE_LOCK();
	msg_set_uint32(msg, AS_EXCHANGE_MSG_ID,
	AS_EXCHANGE_PROTOCOL_IDENTIFIER);
	msg_set_uint64(msg, AS_EXCHANGE_MSG_CLUSTER_KEY, g_exchange.cluster_key);
	msg_set_uint32(msg, AS_EXCHANGE_MSG_TYPE, type);
	EXCHANGE_UNLOCK();
}

/**
 * Get the msg buffer from a pool and fill in all compulsory fields.
 * @return the msg buff with compulsory fields filled in.
 */
static msg*
exchange_msg_get(as_exchange_msg_type type)
{
	msg* msg = as_fabric_msg_get(M_TYPE_EXCHANGE);
	exchange_msg_src_fill(msg, type);
	return msg;
}

/**
 * Return the message buffer back to the pool.
 */
static void
exchange_msg_return(msg* msg)
{
	as_fabric_msg_put(msg);
}

/**
 * Get message id.
 */
static int
exchange_msg_id_get(msg* msg, uint32_t* msg_id)
{
	if (msg_get_uint32(msg, AS_EXCHANGE_MSG_ID, msg_id) != 0) {
		return -1;
	}
	return 0;
}

/**
 * Get message type.
 */
static int
exchange_msg_type_get(msg* msg, as_exchange_msg_type* msg_type)
{
	if (msg_get_uint32(msg, AS_EXCHANGE_MSG_TYPE, msg_type) != 0) {
		return -1;
	}
	return 0;
}

/**
 * Get message cluster key.
 */
static int
exchange_msg_cluster_key_get(msg* msg, as_cluster_key* cluster_key)
{
	if (msg_get_uint64(msg, AS_EXCHANGE_MSG_CLUSTER_KEY, cluster_key) != 0) {
		return -1;
	}
	return 0;
}

/**
 * Get data payload for a message.
 */
static int
exchange_msg_data_payload_get(msg* msg, uint8_t** data_payload,
		size_t* namespaces_payload_size)
{
	if (msg_get_buf(msg, AS_EXCHANGE_MSG_NAMESPACES_PAYLOAD, data_payload,
			namespaces_payload_size, MSG_GET_DIRECT) != 0) {
		return -1;
	}
	return 0;
}

/**
 * Set data payload for a message.
 */
static void
exchange_msg_data_payload_set(msg* msg, uint8_t* data_payload,
		size_t data_payload_size)
{
	if (msg_set_buf(msg, AS_EXCHANGE_MSG_NAMESPACES_PAYLOAD,
			(uint8_t*)data_payload, data_payload_size, MSG_SET_COPY) != 0) {
		CRASH("error setting exchange data payload");
	}
}

/**
 * Check sanity of an incoming message. If this check passes the message is
 * guaranteed to have valid protocol identifier, valid type and valid matching
 * cluster key with source node being a part of the cluster.
 * @return 0 if the message in valid, -1 if the message is invalid and should be
 * ignored.
 */
static bool
exchange_msg_is_sane(cf_node source, msg* msg)
{
	uint32_t id = 0;
	if (exchange_msg_id_get(msg, &id) != 0||
	id != AS_EXCHANGE_PROTOCOL_IDENTIFIER) {
		DEBUG(
				"received exchange message with mismatching identifier - expected %u but was  %u",
				AS_EXCHANGE_PROTOCOL_IDENTIFIER, id);
		return false;
	}

	as_exchange_msg_type msg_type = 0;

	if (exchange_msg_type_get(msg, &msg_type) != 0
			|| msg_type >= AS_EXCHANGE_MSG_TYPE_SENTINEL) {
		WARNING("received exchange message with invalid message type  %u",
				msg_type);
		return false;
	}

	EXCHANGE_LOCK();
	as_cluster_key current_cluster_key = g_exchange.cluster_key;
	bool is_in_cluster = vector_find(&g_exchange.succession_list, &source) >= 0;
	EXCHANGE_UNLOCK();

	if (!is_in_cluster) {
		DEBUG("received exchange message from node %"PRIx64" not in cluster",
				source);
		return false;
	}

	as_cluster_key incoming_cluster_key = 0;
	if (exchange_msg_cluster_key_get(msg, &incoming_cluster_key) != 0
			|| (current_cluster_key != incoming_cluster_key)
			|| current_cluster_key == 0) {
		DEBUG("received exchange message with mismatching cluster key - expected %"PRIx64" but was  %"PRIx64,
				current_cluster_key, incoming_cluster_key);
		return false;
	}

	return true;
}

/**
 * Send a message over fabric.
 *
 * @param msg the message to send.
 * @param dest the desination node.
 * @param error_msg the error message.
 */
static void
exchange_msg_send(msg* msg, cf_node dest, char* error_msg)
{
	if (as_fabric_send(dest, msg, AS_FABRIC_CHANNEL_CTRL)) {
		// Fabric will not return the message to the pool. Do it ourself.
		exchange_msg_return(msg);
		WARNING("%s (dest:%"PRIx64")", error_msg, dest);
	}
}

/**
 * Send a message over to a list of destination nodes.
 *
 * @param msg the message to send.
 * @param dests the node list to send the message to.
 * @param num_dests the number of destination nodes.
 * @param error_msg the error message.
 */
static void
exchange_msg_send_list(msg* msg, cf_node* dests, int num_dests, char* error_msg)
{
	if (as_fabric_send_list(dests, num_dests, msg, AS_FABRIC_CHANNEL_CTRL)
			!= 0) {
		// Fabric will not return the message to the pool. Do it ourself.
		exchange_msg_return(msg);
		as_clustering_log_cf_node_array(CF_WARNING, AS_EXCHANGE, error_msg,
				dests, num_dests);
	}
}

/**
 * Send a commit message to a destination node.
 * @param dest the destination node.
 */
static void
exchange_commit_msg_send(cf_node dest)
{
	msg* commit_msg = exchange_msg_get(AS_EXCHANGE_MSG_TYPE_COMMIT);
	DEBUG("sending commit message to node %"PRIx64, dest);
	exchange_msg_send(commit_msg, dest, "error sending commit message");
}

/**
 * Send a commit message to a list of destination nodes.
 * @param dests the destination nodes.
 * @param num_dests the number of destination nodes.
 */
static void
exchange_commit_msg_send_all(cf_node* dests, int num_dests)
{
	msg* commit_msg = exchange_msg_get(AS_EXCHANGE_MSG_TYPE_COMMIT);
	as_clustering_log_cf_node_array(CF_DEBUG, AS_EXCHANGE,
			"sending commit message to nodes:", dests, num_dests);
	exchange_msg_send_list(commit_msg, dests, num_dests,
			"error sending commit message");
}

/**
 * Send ready to commit message to the principal.
 */
static void
exchange_ready_to_commit_msg_send()
{
	EXCHANGE_LOCK();
	g_exchange.ready_to_commit_send_ts = cf_getms();
	cf_node principal = g_exchange.principal;
	EXCHANGE_UNLOCK();

	msg* ready_to_commit_msg = exchange_msg_get(
			AS_EXCHANGE_MSG_TYPE_READY_TO_COMMIT);
	DEBUG("sending ready to commit message to node %"PRIx64, principal);
	exchange_msg_send(ready_to_commit_msg, principal,
			"error sending ready to commit message");
}

/**
 * Send exchange data to all nodes that have not acked the send.
 */
static void
exchange_data_msg_send_pending_ack()
{
	EXCHANGE_LOCK();
	g_exchange.send_ts = cf_getms();

	cf_node* unacked_nodes;
	int num_unacked_nodes;
	cf_vector* unacked_nodes_vector = cf_vector_stack_create(cf_node);

	exchange_nodes_find_send_unacked(unacked_nodes_vector);
	cf_vector_to_stack_array(unacked_nodes_vector, &unacked_nodes,
			&num_unacked_nodes);

	cf_vector_destroy(unacked_nodes_vector);

	if (!num_unacked_nodes) {
		goto Exit;
	}

	msg* data_msg = exchange_msg_get(AS_EXCHANGE_MSG_TYPE_DATA);
	exchange_msg_data_payload_set(data_msg, g_exchange.self_data_dyn_buf.buf,
			g_exchange.self_data_dyn_buf.used_sz);

	as_clustering_log_cf_node_array(CF_DEBUG, AS_EXCHANGE,
			"sending exchange data to nodes:", unacked_nodes,
			num_unacked_nodes);

	exchange_msg_send_list(data_msg, unacked_nodes, num_unacked_nodes,
			"error sending exchange data");
Exit:
	EXCHANGE_UNLOCK();
}

/**
 * Send a commit message to a destination node.
 * @param dest the destination node.
 */
static void
exchange_data_ack_msg_send(cf_node dest)
{
	msg* ack_msg = exchange_msg_get(AS_EXCHANGE_MSG_TYPE_DATA_ACK);
	DEBUG("sending data ack message to node %"PRIx64, dest);
	exchange_msg_send(ack_msg, dest, "error sending data ack message");
}

/*
 * ----------------------------------------------------------------------------
 * Data payload related
 * ----------------------------------------------------------------------------
 */

/**
 * Add a pid to the namespace hash for the input vinfo.
 */
static void
exchange_namespace_hash_pid_add(shash* ns_hash, as_partition_version* vinfo,
		uint16_t pid)
{
	if (as_partition_version_is_null(vinfo)) {
		// Ignore NULL vinfos.
		return;
	}

	cf_vector* pid_vector;

	// Append the hash.
	if (SHASH_GET_OR_DIE(ns_hash, vinfo, &pid_vector,
			"error reading the namespace hash") != SHASH_OK) {
		// We are seeing this vinfo for the first time.
		pid_vector = cf_vector_create(sizeof(uint16_t),
		AS_EXCHANGE_VINFO_NUM_PIDS_AVG, 0);
		SHASH_PUT_OR_DIE(ns_hash, vinfo, &pid_vector,
				"error adding the the namespace hash");
	}

	cf_vector_append(pid_vector, &pid);
}

/**
 * Destroy the pid vector for each vinfo.
 */
static int
exchange_namespace_hash_destroy_reduce(const void* key, void* data, void* udata)
{
	cf_vector* pid_vector = *(cf_vector**)data;
	cf_vector_destroy(pid_vector);
	return SHASH_REDUCE_DELETE;
}

/**
 * Serialize each vinfo and accumulated pids to the input buffer.
 */
static int
exchange_namespace_hash_serialize_reduce(const void* key, void* data,
		void* udata)
{
	const as_partition_version* vinfo = (const as_partition_version*)key;
	cf_vector* pid_vector = *(cf_vector**)data;
	cf_dyn_buf* dyn_buf = (cf_dyn_buf*)udata;

	// Append the vinfo.
	cf_dyn_buf_append_buf(dyn_buf, (uint8_t*)vinfo, sizeof(*vinfo));

	// Append the count of pids.
	uint32_t num_pids = cf_vector_size(pid_vector);
	cf_dyn_buf_append_buf(dyn_buf, (uint8_t*)&num_pids, sizeof(num_pids));

	// Append each pid.
	for (int i = 0; i < num_pids; i++) {
		uint16_t* pid = cf_vector_getp(pid_vector, i);
		cf_dyn_buf_append_buf(dyn_buf, (uint8_t*)pid, sizeof(*pid));
	}

	return SHASH_OK;
}

/**
 * Append namespace payload, in as_exchange_namespace_payload format, for a
 * namespace to the dynamic buffer.
 *
 * @param ns the namespace.
 * @param dyn_buf the dynamic buffer.
 */
static void
exchange_data_namespace_payload_add(as_namespace* ns, cf_dyn_buf* dyn_buf)
{
	// A hash from each unique non null vinfo to a vector of partition ids
	// having the vinfo.
	shash* ns_hash;

	if (shash_create(&ns_hash, exchange_vinfo_shash,
			sizeof(as_partition_version), sizeof(cf_vector*),
			AS_EXCHANGE_UNIQUE_VINFO_MAX_SIZE_SOFT, 0) != SHASH_OK) {
		CRASH("error creating namespace payload hash");
	}

	as_partition* partitions = ns->partitions;

	// Populate the hash with one entry for each vinfo
	for (int i = 0; i < AS_PARTITIONS; i++) {
		as_partition_version* current_vinfo = &partitions[i].version;
		exchange_namespace_hash_pid_add(ns_hash, current_vinfo, i);
	}

	// We are ready to populate the dyn buffer with this ns's data.
	DEBUG("namespace %s has %d unique vinfos", ns->name,
			shash_get_size(ns_hash));

	// Append the name with a null terminator.
	cf_dyn_buf_append_buf(dyn_buf, (uint8_t*)ns->name, sizeof(ns->name));

	// Append the vinfo count.
	uint32_t num_vinfos = shash_get_size(ns_hash);
	cf_dyn_buf_append_buf(dyn_buf, (uint8_t*)&num_vinfos, sizeof(num_vinfos));

	// Append vinfos and partitions.
	shash_reduce(ns_hash, exchange_namespace_hash_serialize_reduce, dyn_buf);

	// Destroy the intermediate hash and the pid vectors.
	shash_reduce_delete(ns_hash, exchange_namespace_hash_destroy_reduce, NULL);

	shash_destroy(ns_hash);
}

/**
 * Prepare the exchanged data payloads.
 */
static void
exchange_data_payloads_prepare()
{
	EXCHANGE_LOCK();

	// Block / abort migrations and freeze the partition version infos.
	as_partition_balance_disallow_migrations();
	as_partition_balance_synchronize_migrations();

	// Reset the data size for the dyn buffer.
	g_exchange.self_data_dyn_buf.used_sz = 0;

	// Append the number of namespaces (in host order).
	uint32_t num_namespaces = g_config.n_namespaces;
	cf_dyn_buf_append_buf(&g_exchange.self_data_dyn_buf,
			(uint8_t*)&num_namespaces, sizeof(num_namespaces));

	for (int i = 0; i < g_config.n_namespaces; i++) {
		// Append payload for each namespace.
		as_namespace* ns = g_config.namespaces[i];
		exchange_data_namespace_payload_add(ns, &g_exchange.self_data_dyn_buf);
	}

	EXCHANGE_UNLOCK();
}

/**
 * Basic validation for incoming namespace payload.
 * Validates that
 * 	1. Number of vinfos < AS_PARTITIONS.
 * 	2. Each partition is between 0 and AS_PARTITIONS.
 * 	3. Namespaces payload does not exceed payload_end_ptr.
 *
 * @param ns_payload pointer to start of the namespace payload.
 * @param payload_end_ptr end pointer (inclusive) beyond which this namespace
 * data should not span.
 * @param ns_payload_size (output) the size of the input namespace payload. Will
 * be set only if this namespace payload is valid.
 * @return true if this is a valid payload.
 */
static bool
exchange_namespace_payload_is_valid(as_exchange_namespace_payload* ns_payload,
		uint8_t* payload_end_ptr, size_t* ns_payload_size)
{
	if ((uint8_t*)&ns_payload->name > payload_end_ptr) {
		return false;
	}

	int ns_name_len = strnlen(ns_payload->name, AS_ID_NAMESPACE_SZ);

	if (ns_name_len == AS_ID_NAMESPACE_SZ) {
		// The namespace length is too long, abort.
		return false;
	}

	if ((uint8_t*)&ns_payload->num_vinfos > payload_end_ptr) {
		return false;
	}

	if (ns_payload->num_vinfos > AS_PARTITIONS) {
		return false;
	}

	uint8_t* read_ptr = (uint8_t*)ns_payload->vinfos;
	for (int i = 0; i < ns_payload->num_vinfos; i++) {
		if (read_ptr > payload_end_ptr) {
			return false;
		}

		as_exchange_vinfo_payload* vinfo_payload =
				(as_exchange_vinfo_payload*)read_ptr;

		if ((uint8_t*)&vinfo_payload->num_pids > payload_end_ptr) {
			return false;
		}

		if (vinfo_payload->num_pids > AS_PARTITIONS) {
			return false;
		}

		for (int j = 0; j < vinfo_payload->num_pids; j++) {
			uint16_t* pid = &vinfo_payload->pids[j];
			if ((uint8_t*)pid > payload_end_ptr
					|| (uint8_t*)pid + sizeof(*pid) - 1 > payload_end_ptr) {
				return false;
			}

			if (*pid >= AS_PARTITIONS) {
				return false;
			}
		}

		read_ptr += sizeof(as_exchange_vinfo_payload)
				+ vinfo_payload->num_pids * sizeof(uint16_t);
	}

	*ns_payload_size = read_ptr - (uint8_t*)ns_payload;
	return true;
}

/**
 * Basic validation for incoming data playload.
 * Validates that
 * 	1. The payload fits sizewise an as_exchange_namespaces_payload
 * 	2. The number of namespaces fit the maximum limit.
 * 	3. Basic namespace payload validation.
 */
static bool
exchange_data_payload_is_valid(uint8_t* payload, size_t payload_size)
{
	uint8_t* payload_end_ptr = payload + payload_size - 1;
	as_exchange_namespaces_payload* namespaces_payload =
			(as_exchange_namespaces_payload *)payload;

	if (payload + sizeof(*namespaces_payload) > payload_end_ptr) {
		if ((uint8_t*)&namespaces_payload->num_namespaces
				+ sizeof(namespaces_payload->num_namespaces) - 1
				> payload_end_ptr || namespaces_payload->num_namespaces > 0) {
			return false;
		}
		else {
			// This is the case where the other node has no namespaces. Allow
			// for now.
		}

	}

	if (namespaces_payload->num_namespaces > AS_NAMESPACE_SZ) {
		return false;
	}

	as_exchange_namespace_payload* namespace_payload =
			&namespaces_payload->namespace_payloads[0];
	for (int i = 0; i < namespaces_payload->num_namespaces; i++) {
		if ((uint8_t*)namespace_payload > payload_end_ptr) {
			return false;
		}
		size_t namespace_payload_size = 0;
		if (!exchange_namespace_payload_is_valid(namespace_payload,
				payload_end_ptr, &namespace_payload_size)) {
			return false;
		}
		// Jump to the next namespace.
		namespace_payload =
				(as_exchange_namespace_payload*)((uint8_t*)namespace_payload
						+ namespace_payload_size);
	}

	// Return true only if we have payload matching exact input size.
	return (payload_end_ptr - ((uint8_t*)namespace_payload - 1)) == 0;
}

/*
 * ----------------------------------------------------------------------------
 * Common across all states
 * ----------------------------------------------------------------------------
 */

/**
 * Indicates if self node is the cluster principal.
 */
static bool
exchange_self_is_principal()
{
	EXCHANGE_LOCK();
	bool is_principal = (g_config.self_node == g_exchange.principal);
	EXCHANGE_UNLOCK();
	return is_principal;
}

/**
 * Dump exchange state.
 */
static void
exchange_dump(cf_fault_severity severity, bool verbose)
{
	EXCHANGE_LOCK();
	cf_vector* node_vector = cf_vector_stack_create(cf_node);

	char* state_str = "";
	switch (g_exchange.state) {
	case AS_EXCHANGE_STATE_REST:
		state_str = "rest";
		break;
	case AS_EXCHANGE_STATE_EXCHANGING:
		state_str = "exchanging";
		break;
	case AS_EXCHANGE_STATE_READY_TO_COMMIT:
		state_str = "ready to commit";
		break;
	case AS_EXCHANGE_STATE_ORPHANED:
		state_str = "orphaned";
		break;
	}

	LOG(severity, "EXG: state: %s", state_str);

	if (g_exchange.state == AS_EXCHANGE_STATE_ORPHANED) {
		LOG(severity, "EXG: client transactions blocked: %s",
				g_exchange.orphan_state_are_transactions_blocked ?
						"true" : "false");
		LOG(severity, "EXG: orphan since: %"PRIu64"(millis)",
				cf_getms() - g_exchange.orphan_state_start_time);
	}
	else {
		LOG(severity, "EXG: cluster key: %"PRIx64, g_exchange.cluster_key);
		as_clustering_log_cf_node_vector(severity, AS_EXCHANGE,
				"EXG: succession:", &g_exchange.succession_list);

		if (verbose) {
			vector_clear(node_vector);
			exchange_nodes_find_send_unacked(node_vector);
			as_clustering_log_cf_node_vector(severity, AS_EXCHANGE,
					"EXG: send pending:", node_vector);

			vector_clear(node_vector);
			exchange_nodes_find_not_received(node_vector);
			as_clustering_log_cf_node_vector(severity, AS_EXCHANGE,
					"EXG: receive pending:", node_vector);

			if (exchange_self_is_principal()) {
				vector_clear(node_vector);
				exchange_nodes_find_not_ready_to_commit(node_vector);
				as_clustering_log_cf_node_vector(severity, AS_EXCHANGE,
						"EXG: ready to commit pending:", node_vector);
			}
		}
	}

	cf_vector_destroy(node_vector);
	EXCHANGE_UNLOCK();
}

/**
 * Reset state for new round of exchange, while reusing as mush heap allocated
 * space for exchanged data.
 * @param new_succession_list new succession list. Can be NULL for orphaned
 * state.
 * @param new_cluster_key 0 for orphaned state.
 */
static void
exchange_reset_for_new_round(cf_vector* new_succession_list,
		as_cluster_key new_cluster_key)
{
	EXCHANGE_LOCK();
	vector_clear(&g_exchange.succession_list);
	g_exchange.principal = 0;

	if (new_succession_list && cf_vector_size(new_succession_list) > 0) {
		vector_copy(&g_exchange.succession_list, new_succession_list);
		// Set the principal node.
		cf_vector_get(&g_exchange.succession_list, 0, &g_exchange.principal);
		g_exchange.cluster_size = cf_vector_size(new_succession_list);
	}
	else {
		g_exchange.cluster_size = 0;
	}

	// Reset accumulated node states.
	exchange_node_states_reset();

	g_exchange.cluster_key = new_cluster_key;
	EXCHANGE_UNLOCK();
}

/**
 * Receive an orphaned event and abort current round.
 */
static void
exchange_orphaned_handle(as_clustering_event* orphaned_event)
{
	DEBUG("got orphaned event");

	EXCHANGE_LOCK();

	if (g_exchange.state != AS_EXCHANGE_STATE_REST
			&& g_exchange.state != AS_EXCHANGE_STATE_ORPHANED) {
		INFO("aborting partition exchange with cluster key %"PRIx64,
				g_exchange.cluster_key);
	}

	g_exchange.state = AS_EXCHANGE_STATE_ORPHANED;
	exchange_reset_for_new_round(NULL, 0);

	// Stop ongoing migrations if any.
	as_partition_balance_disallow_migrations();
	as_partition_balance_synchronize_migrations();

	// We have not yet blocked transactions for this orphan transition.
	g_exchange.orphan_state_are_transactions_blocked = false;
	// Update the time this node got into orphan state.
	g_exchange.orphan_state_start_time = cf_getms();

	EXCHANGE_UNLOCK();
}

/**
 * Receive a cluster change event and start a new data exchange round.
 */
static void
exchange_cluster_change_handle(as_clustering_event* clustering_event)
{
	EXCHANGE_LOCK();

	DEBUG("got cluster change event");

	if (g_exchange.state != AS_EXCHANGE_STATE_REST
			&& g_exchange.state != AS_EXCHANGE_STATE_ORPHANED) {
		INFO("aborting partition exchange with cluster key %"PRIx64,
				g_exchange.cluster_key);
	}

	exchange_reset_for_new_round(clustering_event->succession_list,
			clustering_event->cluster_key);

	g_exchange.state = AS_EXCHANGE_STATE_EXCHANGING;

	INFO("data exchange started with cluster key %"PRIx64,
			g_exchange.cluster_key);

	// Prepare the data payloads.
	exchange_data_payloads_prepare();

	EXCHANGE_UNLOCK();

	exchange_data_msg_send_pending_ack();
}

/**
 * Handle a cluster change event.
 * @param cluster_change_event the cluster change event.
 */
static void
exchange_clustering_event_handle(as_exchange_event* exchange_clustering_event)
{
	as_clustering_event* clustering_event =
			exchange_clustering_event->clustering_event;

	switch (clustering_event->type) {
	case AS_CLUSTERING_ORPHANED:
		exchange_orphaned_handle(clustering_event);
		break;
	case AS_CLUSTERING_CLUSTER_CHANGED:
		exchange_cluster_change_handle(clustering_event);
		break;
	}
}

/*
 * ----------------------------------------------------------------------------
 * Orphan state event handling
 * ----------------------------------------------------------------------------
 */
/**
 * The wait time in orphan state after which client transactions and transaction
 * related interactions (e.g. valid partitoion map publishing) should be
 * blocked.
 */
static uint32_t
exchange_orphan_transaction_block_timeout()
{
	// Round up to the nearest 5 second interval.
	int round_up_to = 5000;

	int timeout =
			as_clustering_quantum_interval() * AS_EXCHANGE_TRANSACTION_BLOCK_ORPHAN_INTERVALS;
	return ((timeout + round_up_to - 1) / round_up_to) * round_up_to;
}

/**
 * Handle the timer event and if we have been an orphan for too long, block
 * client transactions.
 */
static void
exchange_orphan_timer_event_handle()
{
	bool invoke_transaction_block = false;
	uint32_t timeout = exchange_orphan_transaction_block_timeout();
	EXCHANGE_LOCK();
	if (!g_exchange.orphan_state_are_transactions_blocked
			&& g_exchange.orphan_state_start_time + timeout < cf_getms()) {
		g_exchange.orphan_state_are_transactions_blocked = true;
		invoke_transaction_block = true;
	}
	EXCHANGE_UNLOCK();

	if (invoke_transaction_block) {
		WARNING(
				"blocking client transactions - in orphan state for more than %d milliseconds!",
				timeout);
		as_partition_balance_revert_to_orphan();
	}
}

/**
 * Event processing in the orphan state.
 */
static void
exchange_orphan_event_handle(as_exchange_event* event)
{
	switch (event->type) {
	case AS_EXCHANGE_EVENT_CLUSTER_CHANGE:
		exchange_clustering_event_handle(event);
		break;
	case AS_EXCHANGE_EVENT_TIMER:
		exchange_orphan_timer_event_handle();
		break;
	default:
		break;
	}
}

/*
 * ----------------------------------------------------------------------------
 * Rest state event handling
 * ----------------------------------------------------------------------------
 */

/**
 * Process a message event when in rest state.
 */
static void
exchange_rest_msg_event_handle(as_exchange_event* msg_event)
{
	EXCHANGE_LOCK();

	if (!exchange_msg_is_sane(msg_event->msg_source, msg_event->msg)) {
		goto Exit;
	}

	as_exchange_msg_type msg_type;
	exchange_msg_type_get(msg_event->msg, &msg_type);

	if (exchange_self_is_principal()
			&& msg_type == AS_EXCHANGE_MSG_TYPE_READY_TO_COMMIT) {
		// The commit message did not make it to the source node, hence it send
		// us the ready to commit message. Resend the commit message.
		DEBUG("received a ready to commit message from %"PRIx64,
				msg_event->msg_source);
		exchange_commit_msg_send(msg_event->msg_source);
	}
	else {
		DEBUG(
				"rest state received unexpected mesage of type %d from node %"PRIx64,
				msg_type, msg_event->msg_source);

	}

Exit:

	EXCHANGE_UNLOCK();
}

/**
 * Event processing in the rest state.
 */
static void
exchange_rest_event_handle(as_exchange_event* event)
{
	switch (event->type) {
	case AS_EXCHANGE_EVENT_CLUSTER_CHANGE:
		exchange_clustering_event_handle(event);
		break;
	case AS_EXCHANGE_EVENT_MSG:
		exchange_rest_msg_event_handle(event);
		break;
	default:
		break;
	}
}

/*
 * ----------------------------------------------------------------------------
 * Exchanging state event handling
 * ----------------------------------------------------------------------------
 */

/**
 * Check to see if all exchange data is sent and received. If so switch to
 * ready_to_commit state.
 */
static void
exchange_exchanging_check_switch_ready_to_commit()
{
	EXCHANGE_LOCK();

	cf_vector* node_vector = cf_vector_stack_create(cf_node);
	bool ready_to_commit = false;

	if (g_exchange.state == AS_EXCHANGE_STATE_REST
			|| g_exchange.cluster_key == 0) {
		goto Exit;
	}

	exchange_nodes_find_send_unacked(node_vector);
	if (cf_vector_size(node_vector) > 0) {
		// We still have unacked exchange send messages.
		goto Exit;
	}

	vector_clear(node_vector);
	exchange_nodes_find_not_received(node_vector);
	if (cf_vector_size(node_vector) > 0) {
		// We still haven't received exchange messages from all nodes in the
		// succession list.
		goto Exit;
	}

	g_exchange.state = AS_EXCHANGE_STATE_READY_TO_COMMIT;

	ready_to_commit = true;

	DEBUG("ready to commit exchange data for cluster key %"PRIx64,
			g_exchange.cluster_key);

Exit:
	cf_vector_destroy(node_vector);
	EXCHANGE_UNLOCK();

	if (ready_to_commit) {
		exchange_ready_to_commit_msg_send();
	}
}

/**
 * Handle incoming data message.
 *
 * Assumes the message has been checked for sanity.
 */
static void
exchange_exchanging_data_msg_handle(as_exchange_event* msg_event)
{
	EXCHANGE_LOCK();

	DEBUG("received exchange data from node %"PRIx64, msg_event->msg_source);

	as_exchange_node_state node_state;
	exchange_node_state_get_safe(msg_event->msg_source, &node_state);

	if (!node_state.received) {
		uint8_t* data_payload = NULL;
		size_t data_payload_size = 0;
		if (exchange_msg_data_payload_get(msg_event->msg, &data_payload,
				&data_payload_size) != 0
				|| !exchange_data_payload_is_valid(data_payload,
						data_payload_size)) {
			WARNING("received invalid exchange data payload from node %"PRIx64,
					msg_event->msg_source);
			goto Exit;
		}

		// Copy over the payload to the source node's state.
		if (node_state.data.data_capacity < data_payload_size) {
			// Round up to nearest multiple of 1024 bytes.
			size_t allocate_size = ((data_payload_size + 1023) / 1024) * 1024;

			// Creating an alias to prevent cf_realloc params from being split.
			// ASM build fails if cf_realloc arguments are split.
			void *data_ptr = node_state.data.data;
			node_state.data.data = cf_realloc(data_ptr, allocate_size);
			if (!node_state.data.data) {
				CRASH(
						"error allocating data payload space %zu for source node %"PRIx64,
						allocate_size, msg_event->msg_source);
			}
			node_state.data.data_capacity = allocate_size;
		}

		memcpy(node_state.data.data, data_payload, data_payload_size);
		node_state.data.data_size = data_payload_size;

		// Mark exchange data received from the source.
		node_state.received = true;
		exchange_node_state_update(msg_event->msg_source, &node_state);
	}
	else {
		// Duplicate pinfo received. Ignore.
		INFO("received duplicate exchange data from node %"PRIx64,
				msg_event->msg_source);
	}

	// Send an acknowledgement.
	exchange_data_ack_msg_send(msg_event->msg_source);

	// Check if we can switch to ready to commit state.
	exchange_exchanging_check_switch_ready_to_commit();

Exit:
	EXCHANGE_UNLOCK();
}

/**
 * Handle incoming data ack message.
 *
 * Assumes the message has been checked for sanity.
 */
static void
exchange_exchanging_data_ack_msg_handle(as_exchange_event* msg_event)
{
	EXCHANGE_LOCK();

	DEBUG("received exchange data ack from node %"PRIx64,
			msg_event->msg_source);

	as_exchange_node_state node_state;
	exchange_node_state_get_safe(msg_event->msg_source, &node_state);

	if (!node_state.send_acked) {
		// Mark send as acked in the node state.
		node_state.send_acked = true;
		exchange_node_state_update(msg_event->msg_source, &node_state);
	}
	else {
		// Duplicate ack. Ignore.
		DEBUG("received duplicate data ack from node %"PRIx64,
				msg_event->msg_source);
	}

	// We might have send and received all partition info. Check for completion.
	exchange_exchanging_check_switch_ready_to_commit();

	EXCHANGE_UNLOCK();
}

/**
 * Process a message event when in exchanging state.
 */
static void
exchange_exchanging_msg_event_handle(as_exchange_event* msg_event)
{
	EXCHANGE_LOCK();

	if (!exchange_msg_is_sane(msg_event->msg_source, msg_event->msg)) {
		goto Exit;
	}

	as_exchange_msg_type msg_type;
	exchange_msg_type_get(msg_event->msg, &msg_type);

	switch (msg_type) {
	case AS_EXCHANGE_MSG_TYPE_DATA:
		exchange_exchanging_data_msg_handle(msg_event);
		break;
	case AS_EXCHANGE_MSG_TYPE_DATA_ACK:
		exchange_exchanging_data_ack_msg_handle(msg_event);
		break;
	default:
		DEBUG(
				"exchanging state received unexpected mesage of type %d from node %"PRIx64,
				msg_type, msg_event->msg_source);
	}
Exit:
	EXCHANGE_UNLOCK();
}

/**
 * Process a message event when in exchanging state.
 */
static void
exchange_exchanging_timer_event_handle(as_exchange_event* msg_event)
{
	EXCHANGE_LOCK();
	bool send_data = false;

	cf_clock now = cf_getms();

	// The timeout is a "linear" step function, where the timeout is constant
	// for the step interval.
	cf_clock min_timeout = EXCHANGE_SEND_MIN_TIMEOUT();
	cf_clock max_timeout = EXCHANGE_SEND_MAX_TIMEOUT();
	uint32_t step_interval = EXCHANGE_SEND_STEP_INTERVAL();
	cf_clock timeout =
			MAX(min_timeout,
					MIN(max_timeout, min_timeout * ((now - g_exchange.send_ts) / step_interval)));

	if (g_exchange.send_ts + timeout < now) {
		send_data = true;
	}

	EXCHANGE_UNLOCK();

	if (send_data) {
		exchange_data_msg_send_pending_ack();
	}
}

/**
 * Event processing in the exchanging state.
 */
static void
exchange_exchanging_event_handle(as_exchange_event* event)
{
	switch (event->type) {
	case AS_EXCHANGE_EVENT_CLUSTER_CHANGE:
		exchange_clustering_event_handle(event);
		break;
	case AS_EXCHANGE_EVENT_MSG:
		exchange_exchanging_msg_event_handle(event);
		break;
	case AS_EXCHANGE_EVENT_TIMER:
		exchange_exchanging_timer_event_handle(event);
		break;
	}
}

/*
 * ----------------------------------------------------------------------------
 * Ready_To_Commit state event handling
 * ----------------------------------------------------------------------------
 */

/**
 * Handle incoming ready to commit message.
 *
 * Assumes the message has been checked for sanity.
 */
static void
exchange_ready_to_commit_rtc_msg_handle(as_exchange_event* msg_event)
{
	if (!exchange_self_is_principal()) {
		WARNING(
				"non-principal self received ready to commit message from %"PRIx64" - ignoring",
				msg_event->msg_source);
		return;
	}

	EXCHANGE_LOCK();

	DEBUG("received ready to commit from node %"PRIx64, msg_event->msg_source);

	as_exchange_node_state node_state;
	exchange_node_state_get_safe(msg_event->msg_source, &node_state);

	if (!node_state.is_ready_to_commit) {
		// Mark as ready to commit in the node state.
		node_state.is_ready_to_commit = true;
		exchange_node_state_update(msg_event->msg_source, &node_state);
	}
	else {
		// Duplicate ready to commit received. Ignore.
		INFO("received duplicate ready to commit message from node %"PRIx64,
				msg_event->msg_source);
	}

	cf_vector* node_vector = cf_vector_stack_create(cf_node);
	exchange_nodes_find_not_ready_to_commit(node_vector);

	if (cf_vector_size(node_vector) <= 0) {
		// Send a commit message to all nodes in succession list.
		cf_node* node_list = NULL;
		int num_node_list = 0;
		cf_vector_to_stack_array(&g_exchange.succession_list, &node_list,
				&num_node_list);
		exchange_commit_msg_send_all(node_list, num_node_list);
	}

	cf_vector_destroy(node_vector);

	EXCHANGE_UNLOCK();
}

/**
 * Commit namespace payload for a node.
 * Assumes the namespace vinfo and succession list have been zero set before.
 */
static void
exchange_namespace_payload_commit_for_node(cf_node node,
		as_exchange_namespace_payload* ns_payload, size_t* ns_payload_size)
{
	as_namespace* ns = as_namespace_get_byname(ns_payload->name);
	if (ns == NULL) {
		// Self node does not have this namespace. Maybe its a rolling namespace
		// addition.
		WARNING(
				"ignoring unknown namespace %s in partition info from node %"PRIx64,
				ns_payload->name, node);

		// We should update the namespace payload size either way even if this
		// namespace is ignored.
		uint8_t* read_ptr = (uint8_t*)ns_payload->vinfos;
		for (int i = 0; i < ns_payload->num_vinfos; i++) {
			as_exchange_vinfo_payload* vinfo_payload =
					(as_exchange_vinfo_payload*)read_ptr;

			read_ptr += sizeof(as_exchange_vinfo_payload)
					+ vinfo_payload->num_pids * sizeof(uint16_t);
		}

		*ns_payload_size = read_ptr - (uint8_t*)ns_payload;
		return;
	}

	// Append this node to the namespace succession list.
	int node_ns_succession_index = ns->cluster_size;
	ns->succession[node_ns_succession_index] = node;

	// Increment the ns cluster size.
	ns->cluster_size++;

	uint8_t* read_ptr = (uint8_t*)ns_payload->vinfos;

	for (int i = 0; i < ns_payload->num_vinfos; i++) {
		as_exchange_vinfo_payload* vinfo_payload =
				(as_exchange_vinfo_payload*)read_ptr;

		for (int j = 0; j < vinfo_payload->num_pids; j++) {
			// FIXME - just copy struct instead of memcpy()!
			memcpy(
					&ns->cluster_versions[node_ns_succession_index][vinfo_payload->pids[j]],
					&vinfo_payload->vinfo, sizeof(vinfo_payload->vinfo));

		}

		read_ptr += sizeof(as_exchange_vinfo_payload)
				+ vinfo_payload->num_pids * sizeof(uint16_t);
	}

	DEBUG("committed data from node %"PRIx64" for namespace %s", node, ns->name);
	*ns_payload_size = read_ptr - (uint8_t*)ns_payload;
}

/**
 * Commit exchange data for a given node.
 */
static void
exchange_data_commit_for_node(cf_node node)
{
	EXCHANGE_LOCK();
	as_exchange_node_state node_state;
	exchange_node_state_get_safe(node, &node_state);

	as_exchange_namespaces_payload* namespaces_payload = node_state.data.data;
	as_exchange_namespace_payload* namespace_payload =
			&namespaces_payload->namespace_payloads[0];

	for (int i = 0; i < namespaces_payload->num_namespaces; i++) {
		size_t namespace_payload_size = 0;
		exchange_namespace_payload_commit_for_node(node, namespace_payload,
				&namespace_payload_size);
		// Jump to the next namespace.
		namespace_payload =
				(as_exchange_namespace_payload*)((uint8_t*)namespace_payload
						+ namespace_payload_size);
	}

	EXCHANGE_UNLOCK();
}

/**
 * Commit accumulated exchange data.
 */
static void
exchange_data_commit()
{
	EXCHANGE_LOCK();

	INFO("data exchange completed with cluster key %"PRIx64,
			g_exchange.cluster_key);

	// Reset exchange data for all namespaces.
	for (int i = 0; i < g_config.n_namespaces; i++) {
		as_namespace* ns = g_config.namespaces[i];
		memset(ns->succession, 0, sizeof(ns->succession));

		// Assuming zero to represent "null" partition.
		memset(ns->cluster_versions, 0, sizeof(ns->cluster_versions));

		// Reset ns cluster size to zero.
		ns->cluster_size = 0;
	}

	// Fill the namespace partition version info in succession list order.
	int num_nodes = cf_vector_size(&g_exchange.succession_list);
	for (int i = 0; i < num_nodes; i++) {
		cf_node node;
		cf_vector_get(&g_exchange.succession_list, i, &node);
		exchange_data_commit_for_node(node);
	}

	// Exchange is done, use the current cluster details as the committed
	// cluster details.
	g_exchange.committed_cluster_key = g_exchange.cluster_key;
	g_exchange.committed_cluster_size = g_exchange.cluster_size;
	g_exchange.committed_principal = g_exchange.principal;
	vector_clear(&g_exchange.committed_succession_list);
	vector_copy(&g_exchange.committed_succession_list,
			&g_exchange.succession_list);

	as_partition_balance();

	EXCHANGE_UNLOCK();
}

/**
 * Handle incoming data ack message.
 *
 * Assumes the message has been checked for sanity.
 */
static void
exchange_ready_to_commit_commit_msg_handle(as_exchange_event* msg_event)
{
	EXCHANGE_LOCK();

	if (msg_event->msg_source != g_exchange.principal) {
		WARNING(
				"ignoring commit message from node %"PRIx64" - expected message from %"PRIx64,
				msg_event->msg_source, g_exchange.principal);
		goto Exit;
	}

	INFO("received commit command from principal node %"PRIx64,
			msg_event->msg_source);

	// Commit exchanged data.
	exchange_data_commit();

	// Move to the rest state.
	g_exchange.state = AS_EXCHANGE_STATE_REST;

	// Queue up a cluster change event for downstream sub systems.
	as_exchange_cluster_changed_event cluster_change_event;
	cluster_change_event.cluster_key = g_exchange.committed_cluster_key;
	cluster_change_event.succession = vector_to_array(
			&g_exchange.committed_succession_list);
	cluster_change_event.cluster_size = g_exchange.committed_cluster_size;

	exchange_external_event_queue(&cluster_change_event);

Exit:
	EXCHANGE_UNLOCK();
}

/**
 * Handle incoming data message in ready to commit stage.
 *
 * Assumes the message has been checked for sanity.
 */
static void
exchange_ready_to_commit_data_msg_handle(as_exchange_event* msg_event)
{
	EXCHANGE_LOCK();

	DEBUG("received exchange data from node %"PRIx64, msg_event->msg_source);

	// The source must have missed self node's data ack. Send an
	// acknowledgement.
	exchange_data_ack_msg_send(msg_event->msg_source);

	EXCHANGE_UNLOCK();
}

/**
 * Process a message event when in ready_to_commit state.
 */
static void
exchange_ready_to_commit_msg_event_handle(as_exchange_event* msg_event)
{
	EXCHANGE_LOCK();

	if (!exchange_msg_is_sane(msg_event->msg_source, msg_event->msg)) {
		goto Exit;
	}

	as_exchange_msg_type msg_type;
	exchange_msg_type_get(msg_event->msg, &msg_type);

	switch (msg_type) {
	case AS_EXCHANGE_MSG_TYPE_READY_TO_COMMIT:
		exchange_ready_to_commit_rtc_msg_handle(msg_event);
		break;
	case AS_EXCHANGE_MSG_TYPE_COMMIT:
		exchange_ready_to_commit_commit_msg_handle(msg_event);
		break;
	case AS_EXCHANGE_MSG_TYPE_DATA:
		exchange_ready_to_commit_data_msg_handle(msg_event);
		break;
	default:
		DEBUG(
				"ready to commit state received unexpected message of type %d from node %"PRIx64,
				msg_type, msg_event->msg_source);
	}
Exit:
	EXCHANGE_UNLOCK();
}

/**
 * Process a message event when in ready_to_commit state.
 */
static void
exchange_ready_to_commit_timer_event_handle(as_exchange_event* msg_event)
{
	EXCHANGE_LOCK();

	if (g_exchange.ready_to_commit_send_ts + EXCHANGE_READY_TO_COMMIT_TIMEOUT()
			< cf_getms()) {
		// Its been a while since ready to commit has been sent to the
		// principal, retransmit it so that the principal gets it this time and
		// supplies a commit message.
		exchange_ready_to_commit_msg_send();
	}
	EXCHANGE_UNLOCK();
}

/**
 * Event processing in the ready_to_commit state.
 */
static void
exchange_ready_to_commit_event_handle(as_exchange_event* event)
{
	switch (event->type) {
	case AS_EXCHANGE_EVENT_CLUSTER_CHANGE:
		exchange_clustering_event_handle(event);
		break;
	case AS_EXCHANGE_EVENT_MSG:
		exchange_ready_to_commit_msg_event_handle(event);
		break;
	case AS_EXCHANGE_EVENT_TIMER:
		exchange_ready_to_commit_timer_event_handle(event);
		break;
	}
}

/*
 * ----------------------------------------------------------------------------
 * Exchange core subsystem
 * ----------------------------------------------------------------------------
 */

/**
 * Dispatch an exchange event inline to the relevant state handler.
 */
static void
exchange_event_handle(as_exchange_event* event)
{
	EXCHANGE_LOCK();

	switch (g_exchange.state) {
	case AS_EXCHANGE_STATE_REST:
		exchange_rest_event_handle(event);
		break;
	case AS_EXCHANGE_STATE_EXCHANGING:
		exchange_exchanging_event_handle(event);
		break;
	case AS_EXCHANGE_STATE_READY_TO_COMMIT:
		exchange_ready_to_commit_event_handle(event);
		break;
	case AS_EXCHANGE_STATE_ORPHANED:
		exchange_orphan_event_handle(event);
		break;
	}

	EXCHANGE_UNLOCK();
}

/**
 * Exchange timer event generator thread, to help with retries and retransmits
 * across all states.
 */
static void*
exchange_timer_thr(void* arg)
{
	as_exchange_event timer_event;
	memset(&timer_event, 0, sizeof(timer_event));
	timer_event.type = AS_EXCHANGE_EVENT_TIMER;

	while (EXCHANGE_IS_RUNNING()) {
		// Wait for a while and retry.
		usleep(EXCHANGE_TIMER_TICK_INTERVAL() * 1000);
		exchange_event_handle(&timer_event);
	}
	return NULL;
}

/**
 * Handle incoming messages from fabric.
 */
static int
exchange_fabric_msg_listener(cf_node source, msg* msg, void* udata)
{
	if (!EXCHANGE_IS_RUNNING()) {
		// Ignore this message.
		DEBUG("exchange stopped - ignoring message from %"PRIx64, source);
		goto Exit;
	}

	as_exchange_event msg_event;
	memset(&msg_event, 0, sizeof(msg_event));
	msg_event.type = AS_EXCHANGE_EVENT_MSG;
	msg_event.msg = msg;
	msg_event.msg_source = source;

	exchange_event_handle(&msg_event);
Exit:
	as_fabric_msg_put(msg);
	return 0;
}

/**
 * Listener for cluster change events from clustering layer.
 */
void
exchange_clustering_event_listener(as_clustering_event* event)
{
	if (!EXCHANGE_IS_RUNNING()) {
		// Ignore this message.
		DEBUG("exchange stopped - ignoring cluster change event");
		return;
	}

	as_exchange_event clustering_event;
	memset(&clustering_event, 0, sizeof(clustering_event));
	clustering_event.type = AS_EXCHANGE_EVENT_CLUSTER_CHANGE;
	clustering_event.clustering_event = event;

	// Dispatch the event.
	exchange_event_handle(&clustering_event);
}

/**
 * Initialize the template to be used for exchange messages.
 */
static void
exchange_msg_init()
{
	// Register fabric exchange msg type with no processing function:
	as_fabric_register_msg_fn(M_TYPE_EXCHANGE, g_exchange_msg_template,
			sizeof(g_exchange_msg_template),
			AS_EXCHANGE_MSG_SCRATCH_SIZE, exchange_fabric_msg_listener, NULL);
}

/**
 * Initialize exchange subsystem.
 */
static void
exchange_init()
{
	if (EXCHANGE_IS_INITIALIZED()) {
		return;
	}

	EXCHANGE_LOCK();

	memset(&g_exchange, 0, sizeof(g_exchange));

	// Start in the orphaned state.
	g_exchange.state = AS_EXCHANGE_STATE_ORPHANED;
	g_exchange.orphan_state_start_time = cf_getms();
	g_exchange.orphan_state_are_transactions_blocked = true;

	// Initialize the adjacencies.
	if (shash_create(&g_exchange.nodeid_to_node_state, cf_nodeid_shash_fn,
			sizeof(cf_node), sizeof(as_exchange_node_state),
			AS_EXCHANGE_CLUSTER_MAX_SIZE_SOFT, 0) != SHASH_OK) {
		CRASH("error creating node state hash");
	}

	cf_vector_init(&g_exchange.succession_list, sizeof(cf_node),
	AS_EXCHANGE_CLUSTER_MAX_SIZE_SOFT, VECTOR_FLAG_INITZERO);
	cf_vector_init(&g_exchange.committed_succession_list, sizeof(cf_node),
	AS_EXCHANGE_CLUSTER_MAX_SIZE_SOFT, VECTOR_FLAG_INITZERO);

	// Initialize fabric message pool.
	exchange_msg_init();

	// Initialize self exchange data dynamic buffer.
	cf_dyn_buf_init_heap(&g_exchange.self_data_dyn_buf,
	AS_EXCHANGE_SELF_DYN_BUF_SIZE());

	// Initialize external event publishing.
	exchange_external_event_publisher_init();

	// Get partition versions from storage.
	as_partition_balance_init();

	DEBUG("exchange module initialized");

	EXCHANGE_UNLOCK();
}

/**
 * Stop exchange subsystem.
 */
static void
exchange_stop()
{
	if (!EXCHANGE_IS_RUNNING()) {
		WARNING("exchange is already stopped");
		return;
	}

	// Ungaurded state, but this should be ok.
	g_exchange.sys_state = AS_EXCHANGE_SYS_STATE_SHUTTING_DOWN;

	// Wait for the relanabce send thread to finish.
	pthread_join(g_exchange.timer_tid, NULL);

	EXCHANGE_LOCK();

	g_exchange.sys_state = AS_EXCHANGE_SYS_STATE_STOPPED;

	DEBUG("exchange module stopped");

	EXCHANGE_UNLOCK();

	external_event_publisher_stop();
}

/**
 * Start the exchange subsystem.
 */
static void
exchange_start()
{
	EXCHANGE_LOCK();

	if (EXCHANGE_IS_RUNNING()) {
		// Shutdown the exchange subsystem.
		exchange_stop();
	}

	g_exchange.sys_state = AS_EXCHANGE_SYS_STATE_RUNNING;

	// Start the timer thread.
	if (0
			!= pthread_create(&g_exchange.timer_tid, 0, exchange_timer_thr,
					&g_exchange)) {
		CRASH("could not create exchange thread: %s", cf_strerror(errno));
	}

	DEBUG("exchange module started");

	EXCHANGE_UNLOCK();

	exchange_external_event_publisher_start();
}

/*
 * ----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------
 */
/**
 * Initialize exchange subsystem.
 */
void
as_exchange_init()
{
	exchange_init();
}

/**
 * Start exchange subsystem.
 */
void
as_exchange_start()
{
	if (as_new_clustering()) {
		exchange_start();
	}
}

/**
 * Stop exchange subsystem.
 */
void
as_exchange_stop()
{
}

/**
 * Register to receive cluster-changed events.
 * TODO - may replace with simple static list someday.
 */
void
as_exchange_register_listener(as_exchange_cluster_changed_cb cb, void* udata)
{
	exchange_external_event_listener_register(cb, udata);

	if (!as_new_clustering()) {
		as_paxos_register_change_callback(cb, udata);
	}
}

/**
 * Dump exchange state to log.
 */
void
as_exchange_dump(bool verbose)
{
	if (as_new_clustering()) {
		exchange_dump(CF_INFO, verbose);
	}
}

/**
 * Member-access method.
 */
uint64_t
as_exchange_cluster_key()
{
	return (uint64_t)g_exchange.committed_cluster_key;
}

/**
 * TEMPORARY - used by paxos only.
 */
void
as_exchange_cluster_key_set(uint64_t cluster_key)
{
	g_exchange.committed_cluster_key = (as_cluster_key)cluster_key;
}

/**
 * Member-access method.
 */
uint32_t
as_exchange_cluster_size()
{
	return g_exchange.committed_cluster_size;
}

/**
 * Return the committed succession list.
 */
cf_node*
as_exchange_succession()
{
	return vector_to_array(&g_exchange.committed_succession_list);
}

/**
 * Return the committed succession list as a string in a dyn-buf.
 */
void
as_exchange_info_get_succession(cf_dyn_buf* db)
{
	EXCHANGE_LOCK();

	cf_node* nodes = vector_to_array(&g_exchange.committed_succession_list);

	for (uint32_t i = 0; i < g_exchange.committed_cluster_size; i++) {
		cf_dyn_buf_append_uint64_x(db, nodes[i]);
		cf_dyn_buf_append_char(db, ',');
	}

	if (g_exchange.committed_cluster_size != 0) {
		cf_dyn_buf_chomp(db);
	}

	// Always succeeds.
	cf_dyn_buf_append_string(db, "\nok");

	EXCHANGE_UNLOCK();
}

/**
 * TEMPORARY - used by paxos only.
 */
void
as_exchange_succession_set(cf_node* succession, uint32_t cluster_size)
{
	vector_clear(&g_exchange.committed_succession_list);

	for (uint32_t i = 0; i < cluster_size; i++) {
		cf_vector_append(&g_exchange.committed_succession_list, &succession[i]);
	}

	g_exchange.committed_principal = cluster_size > 0 ? succession[0] : 0;
	g_exchange.committed_cluster_size = cluster_size;
}

/**
 * Member-access method.
 */
cf_node
as_exchange_principal()
{
	return g_exchange.committed_principal;
}
