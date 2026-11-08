/*
 * Copyright (c) 2026 Emilio Schinina' <emilioschi@gmail.com>
 *
 * This file is part of libterae.
 *
 * libterae is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * libterae is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with libterae.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file base_server.h
 * @brief The MQTT Server, for MQTT 3.1, MQTT 3.1.1 and MQTT 5.0 on the same
 * listener.
 *
 * The server does no input or output of its own. The caller reports new
 * connections, reads the bytes each one sends into the places
 * mqtt_server_receive_space gives and reports them with
 * mqtt_server_receive_commit, and takes the bytes each one should be sent with
 * mqtt_server_transmit_view and mqtt_server_transmit_consume. Time comes in as
 * an argument. It runs on one thread, with no locks and no calls to an
 * allocator: all memory is provided at start-up in mqtt_server_storage, and
 * running out of any of it is reported to the peer as the protocol allows.
 *
 * Both rings of every connection take their blocks from one pool, so a quiet
 * connection holds no memory for bytes in transit. Each connection has a quota
 * of blocks, and one that finds no room stops being read until blocks come
 * back. base_server_pool.c gives the rules.
 *
 * [V31] MQTT V3.1 Protocol Specification.
 * @li Cited: 3, 4.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015.
 * @li Cited: 3, 4.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3, 4, p.42-117.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */
#ifndef MQTT_BASE_SERVER_H
#define MQTT_BASE_SERVER_H

#include <stdbool.h>
#include <stdint.h>

#include "base_binary_t.h"
#include "base_bool.h"
#include "base_packet.h"
#include "base_page.h"
#include "base_page_arena.h"
#include "base_pool.h"
#include "base_ring.h"
#include "base_slab.h"
#include "base_status.h"
#include "base_store.h"
#include "base_tree.h"
#include "base_utf8_t.h"
#include "base_version.h"

/** @brief The index that stands for nothing in the server's tables. */
#define MQTT_SERVER_NONE 0xFFFFFFFFU
/** @brief Number of bytes in an Assigned Client Identifier. */
#define MQTT_SERVER_ASSIGNED_LENGTH 20U
/**
 * @brief The most runs a Payload received in blocks is passed on in. A packet
 * that would need more goes to the arena whole.
 */
#define MQTT_SERVER_RECEIVE_RUNS 32U
/**
 * @brief Number of connections in a page of their arena. Each is given 256
 * bytes, a power of two at least its size, so a page of them is never more than
 * MQTT_PAGE_SIZE bytes and its number of entries is a power of two.
 */
#define MQTT_SERVER_PAGE_CONNECTIONS (MQTT_PAGE_SIZE / 256U)
/** @brief Number of Sessions in a page of their arena: 128 bytes each. */
#define MQTT_SERVER_PAGE_SESSIONS (MQTT_PAGE_SIZE / 128U)
/** @brief Number of deliveries in a page of their arena: 32 bytes each. */
#define MQTT_SERVER_PAGE_DELIVERIES (MQTT_PAGE_SIZE / 32U)
/** @brief Number of messages in a page of their arena: 64 bytes each. */
#define MQTT_SERVER_PAGE_MESSAGES (MQTT_PAGE_SIZE / 64U)
/** @brief Number of subscriptions in a page of their arena: 64 bytes each. */
#define MQTT_SERVER_PAGE_SUBSCRIPTIONS (MQTT_PAGE_SIZE / 64U)
/** @brief Number of shared groups in a page of their arena: 32 bytes each. */
#define MQTT_SERVER_PAGE_GROUPS (MQTT_PAGE_SIZE / 32U)
/** @brief The most runs a record of the store is made of. */
#define MQTT_SERVER_STORE_RUNS MQTT_STORE_RECORD_RUNS
/**
 * @brief Number of bytes mqtt_server_snapshot_next needs at least: room for
 * the fixed fields of any record. A SUBSCRIBE record needs its Topic Filter's
 * length more, so this and 65,535 always suffice.
 */
#define MQTT_SERVER_SNAPSHOT_SPACE 128U
/**
 * @brief Number of bytes mqtt_server_restore needs at least. A record other
 * than MESSAGE has to fit in what it is given, which a Client Identifier and a
 * Topic Filter of 65,535 bytes each do in MQTT_SERVER_RESTORE_SPACE. At its
 * end, the restore also uses this many as one bit for each Packet Identifier.
 */
#define MQTT_SERVER_RESTORE_MINIMUM 8192U
/** @brief Number of bytes that hold any record mqtt_server_restore reads. */
#define MQTT_SERVER_RESTORE_SPACE ((2U * 65535U) + 128U)
/** @brief The most snapshot copies mqtt_server_restore reads. */
#define MQTT_SERVER_RESTORE_COPIES 8U

/** @brief The state of a Network Connection. */
typedef enum {
    /** @brief The entry is not in use. */
    MQTT_CONNECTION_FREE = 0,
    /** @brief Waiting for the CONNECT packet. */
    MQTT_CONNECTION_WAIT_CONNECT,
    /** @brief In the AUTH exchange that comes before the CONNACK packet. */
    MQTT_CONNECTION_AUTHENTICATING,
    /** @brief The CONNACK packet has accepted the connection. */
    MQTT_CONNECTION_ACTIVE,
    /**
     * @brief The connection will be closed once the bytes waiting have been
     * sent.
     */
    MQTT_CONNECTION_CLOSING
} mqtt_connection_state;

/** @brief What a connection has sent since the network layer last asked. */
typedef enum {
    /** @brief No packet has started. */
    MQTT_SERVER_ACTIVITY_NONE = 0,
    /** @brief Only PINGREQ packets have started. */
    MQTT_SERVER_ACTIVITY_PINGREQ,
    /** @brief A packet other than PINGREQ has started. */
    MQTT_SERVER_ACTIVITY_OTHER
} mqtt_server_activity;

/** @brief One Network Connection. */
typedef struct {
    /**
     * @brief MQTT_BOOL_SET while the connection is open. The interface refuses
     * an index whose entry holds anything else.
     */
    mqtt_bool open;
    /** @brief The state. */
    mqtt_connection_state state;
    /** @brief The protocol in use, once the CONNECT packet has been read. */
    mqtt_version version;
    /** @brief Whole packets waiting to be sent. */
    mqtt_ring ring;
    /** @brief Bytes received and not yet taken, up to a few small packets. */
    mqtt_ring receive;
    /** @brief The Session, or MQTT_SERVER_NONE. */
    uint32_t session;
    /** @brief The time the connection was opened. */
    uint64_t opened_ms;
    /** @brief The time of the last packet received. */
    uint64_t last_received_ms;
    /** @brief How long the connection may stay silent. Zero means no limit. */
    uint64_t keep_alive_limit_ms;
    /** @brief The Keep Alive in force, in seconds, or 0 for none. */
    uint16_t keep_alive;
    /** @brief What has started since mqtt_server_take_activity last ran. */
    mqtt_server_activity activity;
    /** @brief Arena block holding a packet that has only partly arrived. */
    uint32_t assembly_block;
    /** @brief Number of bytes of the packet held so far. */
    uint32_t assembly_used;
    /** @brief Number of bytes in the packet. */
    uint32_t assembly_size;
    /** @brief Number of bytes in the ring that go before the direct payload. */
    uint32_t direct_ring_bytes;
    /** @brief The rest of a payload being sent directly from the arena. */
    const uint8_t *direct_data;
    /** @brief Number of bytes in direct_data. */
    uint32_t direct_left;
    /**
     * @brief The message direct_data belongs to, which is held until it has
     * been sent.
     */
    uint32_t direct_message;
    /** @brief The next message to send again after the session resumed. */
    uint32_t resend;
    /**
     * @brief The next message whose PUBREL to send again after the session
     * resumed.
     */
    uint32_t resend_release;
    /**
     * @brief Number of QoS 1 and QoS 2 messages sent and not yet acknowledged.
     */
    uint32_t on_wire;
    /**
     * @brief When an MQTT 3.1 Client's messages were last sent, as far as a
     * retry is concerned.
     */
    uint64_t retry_last_ms;
    /** @brief How long to wait for the next retry, in milliseconds. */
    uint32_t retry_delay_ms;
    /** @brief The delivery the retry timer was started for. */
    uint32_t retry_first;
    /** @brief The largest packet the Client accepts. */
    uint32_t peer_maximum_packet_size;
    /**
     * @brief How many QoS 1 and QoS 2 messages may be unacknowledged at one
     * time.
     */
    uint32_t peer_window;
    /** @brief The next connection with bytes to send. */
    uint32_t next_pending;
    /** @brief The next connection waiting for blocks. */
    uint32_t next_waiter;
    /**
     * @brief Arena block holding a copy of the CONNECT packet during an AUTH
     * exchange.
     */
    uint32_t connect_block;
    /** @brief Number of bytes in that copy. */
    uint32_t connect_size;
    /** @brief Arena block holding the Authentication Method. */
    uint32_t method_block;
    /**
     * @brief Number of QoS 2 messages received on this connection and not yet
     * released.
     */
    uint32_t incoming_here;
    /** @brief true while a re-authentication the Client started is going on. */
    bool reauthenticating;
    /**
     * @brief true if the CONNECT packet asked for a Session that ends with its
     * connection.
     */
    bool expiry_asked_zero;
    /** @brief Number of bytes in the Authentication Method. */
    uint16_t method_length;
    /**
     * @brief true if the connection is in the list of those with bytes to send.
     */
    bool pending;
    /** @brief true if the connection is waiting for blocks. */
    bool waiting;
    /**
     * @brief true while there is no room to receive anything, so the network
     * layer stops reading and the bytes wait in the kernel.
     */
    bool throttled;
    /**
     * @brief Number of bytes of the ring that may go before an answer that
     * waits for the journal, or MQTT_SERVER_NONE if nothing waits.
     */
    uint32_t hold_bytes;
    /**
     * @brief The low 32 bits of the group that has to be stored before the
     * bytes after hold_bytes may go.
     */
    uint32_t hold_sequence;
    /** @brief The next connection on the list of those with answers held. */
    uint32_t next_held;
    /** @brief true if the connection is on the list of those held. */
    bool held_listed;
} mqtt_connection;

/** @brief What the Server holds for one MQTT Session. */
typedef struct {
    /**
     * @brief The connection, or MQTT_SERVER_NONE if the Client is not
     * connected.
     */
    uint32_t connection;
    /** @brief Arena block holding the Client Identifier. */
    uint32_t identifier_block;
    /** @brief The Session's first subscription. */
    uint32_t subscriptions;
    /** @brief The first message waiting to be sent. */
    uint32_t queued_head;
    /** @brief The last message waiting to be sent. */
    uint32_t queued_tail;
    /** @brief Number of messages waiting to be sent. */
    uint32_t queued_count;
    /** @brief The oldest message sent and not yet acknowledged. */
    uint32_t inflight_head;
    /** @brief The newest message sent and not yet acknowledged. */
    uint32_t inflight_tail;
    /**
     * @brief The first QoS 2 Packet Identifier waiting for its PUBREL packet,
     * the newest, in a list linked both ways.
     */
    uint32_t incoming_head;
    /**
     * @brief Number of entries in the incoming list, at most receive_maximum.
     */
    uint32_t incoming_count;
    /** @brief The Will Message, or MQTT_SERVER_NONE. */
    uint32_t will_message;
    /** @brief Will Delay Interval, in seconds. */
    uint32_t will_delay_s;
    /**
     * @brief Message Expiry Interval of the Will Message, in seconds, if it has
     * one.
     */
    uint32_t will_expiry_s;
    /** @brief Session Expiry Interval, in seconds. */
    uint32_t expiry_s;
    /** @brief The protocol of its last connection. */
    mqtt_version version;
    /**
     * @brief The time the Will Message is due, or zero if no timer is running.
     */
    uint64_t will_due_ms;
    /** @brief The time the Session ends, or zero if no timer is running. */
    uint64_t expiry_due_ms;
    /** @brief The last Packet Identifier used. */
    uint16_t last_packet_identifier;
    /**
     * @brief The last unfinished message from before the values last wrapped
     * around, or none.
     */
    uint32_t identifier_boundary;
    /** @brief Number of bytes in the Client Identifier. */
    uint16_t identifier_length;
    /** @brief true if the Will Message has a Message Expiry Interval. */
    bool will_has_expiry;
    /** @brief true if the entry is in use. */
    bool in_use;
    /**
     * @brief true if the store holds the Session, so its end has to be
     * written.
     */
    bool stored;
} mqtt_session;

/** @brief One message on its way to one Session. */
typedef struct {
    /**
     * @brief The name of the delivery in the store, or zero if the store
     * doesn't hold it.
     */
    uint64_t store_id;
    /** @brief The next entry in the list. */
    uint32_t next;
    /** @brief The previous entry in the list. */
    uint32_t previous;
    /** @brief The message, or MQTT_SERVER_NONE once it is no longer needed. */
    uint32_t message;
    /** @brief The Subscription Identifier, or zero. */
    uint32_t subscription_identifier;
    /** @brief The Packet Identifier. */
    uint16_t packet_identifier;
    /** @brief The QoS it is sent with. */
    uint8_t qos;
    /** @brief The RETAIN flag it is sent with. */
    bool retain;
    /** @brief true if it has been sent before, so the DUP flag is set. */
    bool dup;
    /**
     * @brief true once the PUBREL packet of a QoS 2 message sent to the Client
     * has gone, so that the PUBCOMP packet is what it waits for.
     */
    bool released;
    /**
     * @brief true if its PUBLISH packet was sent on the Session's current
     * connection.
     */
    bool on_wire;
} mqtt_delivery;

/** @brief A retained message, as an application saves and restores it. */
typedef struct {
    /** @brief The Topic Name. */
    mqtt_utf8 topic_name;
    /** @brief The Properties, encoded, without their length. */
    mqtt_payload properties;
    /** @brief The Payload. */
    mqtt_payload payload;
    /**
     * @brief What is left of its Message Expiry Interval in milliseconds, or
     * zero if none.
     */
    uint64_t expiry_left_ms;
    /** @brief The QoS it was published with. */
    uint8_t qos;
} mqtt_server_retained;

/** @brief One stored Application Message. */
typedef struct {
    /**
     * @brief Arena block holding the Topic Name, then the Properties, then the
     * Payload.
     */
    uint32_t block;
    /** @brief Number of holders. */
    uint32_t references;
    /** @brief Number of bytes in the Payload. */
    uint32_t payload_length;
    /** @brief Number of bytes of Properties. */
    uint32_t properties_length;
    /**
     * @brief The time the Message Expiry Interval ends, or zero if it has none.
     */
    uint64_t expiry_ms;
    /**
     * @brief The name of the message in the store, or zero if the store doesn't
     * hold it.
     */
    uint64_t store_id;
    /**
     * @brief The low 32 bits of the generation of the journal segment it was
     * last written to. Each segment writes it again before naming it.
     */
    uint32_t store_generation;
    /** @brief Number of bytes in the Topic Name. */
    uint16_t topic_length;
    /** @brief The QoS it was published with. */
    uint8_t qos;
    /** @brief The RETAIN flag it was published with. */
    bool retain;
} mqtt_message;

/** @brief One subscription of one Session. */
typedef struct {
    /** @brief The node of the tree it is attached to. */
    uint32_t node;
    /** @brief The Session. */
    uint32_t session;
    /** @brief The next subscription at the node or in the group. */
    uint32_t next_here;
    /** @brief The previous subscription at the node or in the group. */
    uint32_t previous_here;
    /** @brief The next subscription of the Session. */
    uint32_t next_of_session;
    /** @brief The previous subscription of the Session. */
    uint32_t previous_of_session;
    /** @brief The shared group, or MQTT_SERVER_NONE. */
    uint32_t group;
    /** @brief The Subscription Identifier, or zero. */
    uint32_t identifier;
    /** @brief The Subscription Options. */
    uint8_t options;
} mqtt_subscription;

/** @brief The Sessions sharing one Shared Subscription. */
typedef struct {
    /** @brief The next group at the node. */
    uint32_t next;
    /** @brief The previous group at the node. */
    uint32_t previous;
    /** @brief Arena block holding the ShareName. */
    uint32_t name_block;
    /** @brief The first member. */
    uint32_t members;
    /** @brief The member that gets the next message. */
    uint32_t cursor;
    /** @brief Number of bytes in the ShareName. */
    uint16_t name_length;
} mqtt_share_group;

/** @brief A Topic Alias the Client has set. */
typedef struct {
    /** @brief Arena block holding the Topic Name, or MQTT_PAGE_ARENA_NONE. */
    uint32_t block;
    /** @brief Number of bytes in the Topic Name. */
    uint16_t length;
} mqtt_topic_alias;

/**
 * @brief Decide whether a Client may connect.
 * @param[in] user_tag The tag from the configuration.
 * @param[in] packet The CONNECT packet.
 * @return MQTT_OK, MQTT_BAD_USER_NAME_OR_PASSWORD, or MQTT_NOT_AUTHORIZED.
 */
typedef mqtt_status (*mqtt_server_authenticate)(uint32_t user_tag,
                                                const mqtt_connect *packet);

/**
 * @brief Take a step of enhanced authentication.
 * @param[in] user_tag The tag from the configuration.
 * @param[in] connection The connection, which the function can use to track the
 * exchange.
 * @param[in] method Authentication Method.
 * @param[in] data_in Authentication Data received.
 * @param[out] data_out Authentication Data to send. It must stay valid until
 * the call returns.
 * @return MQTT_OK if done, MQTT_INCOMPLETE to continue, or a failure.
 */
typedef mqtt_status (*mqtt_server_auth_step)(uint32_t user_tag,
                                             uint32_t connection,
                                             const mqtt_utf8 *method,
                                             const mqtt_binary *data_in,
                                             mqtt_binary *data_out);

/**
 * @brief Decide whether a Client may publish to, or subscribe to, a topic.
 * @param[in] user_tag The tag from the configuration.
 * @param[in] client_identifier The Client Identifier.
 * @param[in] topic The Topic Name or Topic Filter.
 * @return true if it may.
 */
typedef bool (*mqtt_server_authorize)(uint32_t user_tag,
                                      const mqtt_utf8 *client_identifier,
                                      const mqtt_utf8 *topic);

/**
 * @brief Fill bytes from a source of random numbers that an attacker can't
 * predict.
 * @param[in] user_tag The tag from the configuration.
 * @param[out] data Where to put them.
 * @param[in] length Number of bytes.
 * @return MQTT_OK, or any other value if there are none to give.
 */
typedef mqtt_status (*mqtt_server_random_bytes)(uint32_t user_tag,
                                                uint8_t *data,
                                                uint32_t length);

/**
 * @brief How a server behaves. Each limit has the meaning given by the Property
 * of that name.
 */
typedef struct {
    /** @brief Hands to every callback. */
    uint32_t user_tag;
    /**
     * @brief Decides whether a Client may connect, or NULL to let all connect.
     */
    mqtt_server_authenticate authenticate;
    /**
     * @brief Runs enhanced authentication, or NULL to refuse any Authentication
     * Method.
     */
    mqtt_server_auth_step auth_step;
    /**
     * @brief Decides whether a Client may publish, or NULL to let all publish.
     */
    mqtt_server_authorize authorize_publish;
    /**
     * @brief Decides whether a Client may subscribe, or NULL to let all
     * subscribe.
     */
    mqtt_server_authorize authorize_subscribe;
    /**
     * @brief Gives the random bytes of each Assigned Client Identifier, or NULL
     * to number them in order. Numbers let a Client guess another's identifier
     * and take its Session over, so they are also what a failed call leaves.
     */
    mqtt_server_random_bytes random_bytes;
    /** @brief Largest packet accepted from a Client. */
    uint32_t maximum_packet_size;
    /**
     * @brief The largest packet from a Client that has not been accepted yet.
     */
    uint32_t maximum_connect_size;
    /**
     * @brief The most QoS 2 messages a Client may have in progress at once,
     * from 1 to 65535.
     *
     * Those on the current connection count against Receive Maximum: past it,
     * a Client of MQTT 5.0 is disconnected. Those a resumed Session brings
     * with it count too towards what a Session may hold: past that, a new
     * flow gets a PUBREC with Quota exceeded with MQTT 5.0, and before 5.0
     * the connection closes.
     */
    uint32_t receive_maximum;
    /**
     * @brief The most QoS 1 and QoS 2 messages sent to a Client and not yet
     * acknowledged, from 1 to 65535.
     */
    uint32_t maximum_inflight;
    /** @brief The most messages waiting to be sent to one Session. */
    uint32_t maximum_queued;
    /** @brief Upper limit on Session Expiry Interval. */
    uint32_t maximum_session_expiry;
    /**
     * @brief The value to use for Session Expiry Interval with MQTT 3.1 and
     * 3.1.1.
     */
    uint32_t persistent_session_expiry;
    /**
     * @brief Largest Client Identifier accepted, above the 23 bytes all Servers
     * must take.
     */
    uint32_t maximum_client_identifier;
    /**
     * @brief How long a new connection has to send its CONNECT packet and end
     * any authentication, in milliseconds. 0 sets no limit: a connection that
     * never sends one keeps its place until the program closes it.
     */
    uint32_t connect_timeout_ms;
    /**
     * @brief How long to wait before sending an MQTT 3.1 Client's unanswered
     * messages again, in milliseconds, doubling with each retry. MQTT 3.1
     * suggests this and the later versions forbid it. Zero means only to do so
     * when a Session resumes.
     */
    uint32_t retry_interval_ms;
    /** @brief Topic Alias Maximum. */
    uint16_t topic_alias_maximum;
    /**
     * @brief Server Keep Alive, or zero to accept whatever the Client asks for.
     */
    uint16_t server_keep_alive;
    /** @brief Maximum QoS. A value above 2 is taken as 2. */
    uint8_t maximum_qos;
    /** @brief Retain Available. */
    bool retain_available;
    /** @brief Wildcard Subscription Available. */
    bool wildcard_subscription_available;
    /** @brief Subscription Identifier Available. */
    bool subscription_identifier_available;
    /** @brief Shared Subscription Available. */
    bool shared_subscription_available;
    /**
     * @brief true to treat a filter with the share prefix as shared with MQTT
     * 3.1.1 too, as Mosquitto and NanoMQ do. MQTT 3.1.1 gives it no meaning, so
     * by default it is ordinary.
     */
    bool shared_subscription_311;
    /**
     * @brief true to let Clients of MQTT 3.1.1 and 5.0 publish to topics
     * starting with a dollar, which those versions say a Server should prevent.
     * By default they are refused as if not authorized.
     */
    bool dollar_publish_allowed;
    /**
     * @brief Whether to send PUBACK or PUBREC with Reason Code 0x10 when
     * nothing is subscribed.
     */
    bool report_no_matching_subscribers;
    /**
     * @brief Number of blocks a connection may hold while the pool keeps only
     * pool_reserve free.
     */
    uint32_t pool_soft_quota;
    /**
     * @brief The most blocks one connection may hold, which sets the largest
     * packet sent from its ring: about three quarters of this many blocks.
     */
    uint32_t pool_hard_quota;
    /**
     * @brief Free blocks kept for replies and for connections that have just
     * started to need a block.
     */
    uint32_t pool_reserve;
    /**
     * @brief Free blocks kept back from a connection holding pool_soft_quota
     * or more, at least pool_reserve.
     */
    uint32_t pool_borrow_reserve;
    /**
     * @brief Hands each record of the journal to the application, or NULL to
     * keep nothing across a restart.
     */
    mqtt_store_append store_append;
    /**
     * @brief What records about Sessions have to outlast: their subscriptions,
     * Will Messages and lifetimes. MQTT_STORE_MEMORY writes nothing at all.
     */
    mqtt_store_durability store_sessions;
    /**
     * @brief What records about messages have to outlast: those waiting, in
     * flight or retained, and the QoS 2 Packet Identifiers received.
     * MQTT_STORE_MEMORY writes none, so a restart keeps the Sessions without
     * their messages.
     */
    mqtt_store_durability store_messages;
    /**
     * @brief Number of bytes of journal segment after which a snapshot is due,
     * so that the journal starts again.
     */
    uint64_t store_snapshot_bytes;
} mqtt_server_config;

/**
 * @brief All the memory a server uses. The counts set the server's capacity.
 */
typedef struct {
    /** @brief One entry for each connection. */
    mqtt_connection *connections;
    /** @brief Number of connections. */
    uint32_t connection_count;
    /**
     * @brief Bytes of the blocks every connection's rings share:
     * pool_block_count times pool_block_size.
     */
    uint8_t *pool_bytes;
    /** @brief One link for each block. */
    uint32_t *pool_next;
    /** @brief Number of bytes in a block, at most MQTT_POOL_BLOCK_MAX. */
    uint32_t pool_block_size;
    /** @brief Number of blocks. */
    uint32_t pool_block_count;
    /**
     * @brief Where a packet that arrived in several blocks is gathered when it
     * must be in one place. Its size limits the packets kept in blocks: larger
     * ones go to the arena.
     */
    uint8_t *receive_scratch;
    /** @brief Number of bytes in receive_scratch. */
    uint32_t receive_scratch_size;
    /**
     * @brief Topic Aliases: connection_count times topic_alias_maximum, which
     * has to fit in 32 bits.
     */
    mqtt_topic_alias *aliases;
    /** @brief One entry for each Session. */
    mqtt_session *sessions;
    /** @brief Number of Sessions. */
    uint32_t session_count;
    /**
     * @brief The Sessions in order of their Client Identifiers, so that one is
     * found by halving.
     */
    uint32_t *session_table;
    /**
     * @brief Number of entries, at least session_count. Any more are not
     * used.
     */
    uint32_t session_table_count;
    /** @brief One entry for each message on its way to a Session. */
    mqtt_delivery *deliveries;
    /** @brief Number of deliveries. */
    uint32_t delivery_count;
    /** @brief One entry for each stored message. */
    mqtt_message *messages;
    /** @brief Number of messages. */
    uint32_t message_count;
    /** @brief One entry for each subscription. */
    mqtt_subscription *subscriptions;
    /** @brief Number of subscriptions. */
    uint32_t subscription_count;
    /** @brief One entry for each shared group. */
    mqtt_share_group *groups;
    /** @brief Number of shared groups. */
    uint32_t group_count;
    /** @brief The nodes of the topic tree. */
    mqtt_tree_node *nodes;
    /** @brief Number of nodes, at most MQTT_TREE_NODES_MAX. */
    uint32_t node_count;
    /**
     * @brief The branches of the tries of the topic tree, which find a node's
     * children by the bits of their levels. NULL only if branch_count is 0.
     */
    mqtt_tree_branch *branches;
    /**
     * @brief Number of branches, at least node_count - 2, which is the most
     * the nodes can need, and at most MQTT_TREE_NODES_MAX.
     */
    uint32_t branch_count;
    /**
     * @brief The pages of the text of the topic tree's levels:
     * topic_text_pages times MQTT_PAGE_SIZE bytes.
     */
    uint8_t *topic_text;
    /** @brief Number of pages of text. */
    uint32_t topic_text_pages;
    /**
     * @brief The pages of variable-sized data, such as messages and Client
     * Identifiers: arena_pages times MQTT_PAGE_SIZE bytes.
     */
    uint8_t *arena_bytes;
    /** @brief Number of pages of variable-sized data. */
    uint32_t arena_pages;
    /**
     * @brief MQTT_PAGE_META_WORDS words for each page of every arena: as many
     * as mqtt_server_meta_words gives for the rest of this storage.
     */
    uint32_t *page_meta;
    /** @brief Number of words in page_meta. */
    uint32_t page_meta_words;
} mqtt_server_storage;

/** @brief Things counted while a server runs. */
typedef struct {
    /** @brief Connections accepted by CONNACK. */
    uint64_t connections_accepted;
    /** @brief Application Messages received from Clients. */
    uint64_t messages_received;
    /** @brief PUBLISH packets sent to Clients. */
    uint64_t messages_sent;
    /** @brief Messages dropped because a queue or a pool was full. */
    uint64_t messages_dropped;
    /** @brief Connections closed because of an error in a packet. */
    uint64_t protocol_errors;
    /**
     * @brief Messages at QoS 0 that could not be taken, as those publishers
     * can't be told.
     */
    uint64_t messages_refused;
    /**
     * @brief Packets saying why a connection is closing that there was no room
     * to send.
     */
    uint64_t replies_lost;
    /** @brief Times a connection stopped reading for want of room. */
    uint64_t receive_throttled;
    /** @brief Records handed to store_append. */
    uint64_t records_written;
    /** @brief Records store_append refused, each of which broke the journal. */
    uint64_t records_refused;
} mqtt_server_statistics;

/** @brief Where a server's journal stands. */
typedef enum {
    /**
     * @brief Nothing is written: there is no store_append, or Sessions are kept
     * in memory only.
     */
    MQTT_SERVER_STORE_OFF = 0,
    /** @brief Nothing is written until the first snapshot begins. */
    MQTT_SERVER_STORE_FRESH,
    /** @brief Each change is written to the journal. */
    MQTT_SERVER_STORE_RUNNING,
    /**
     * @brief A record was refused, or a sync failed. Nothing more is written,
     * and every answer waits, until a snapshot is whole.
     */
    MQTT_SERVER_STORE_BROKEN
} mqtt_server_store_state;

/**
 * @brief One entry of the table a restore finds stored messages and deliveries
 * in by their store ids.
 */
typedef struct {
    /** @brief The store id. */
    uint64_t id;
    /** @brief The message or the delivery, or MQTT_SERVER_NONE once gone. */
    uint32_t index;
    /** @brief The Session of a delivery, or MQTT_SERVER_NONE for a message. */
    uint32_t session;
} mqtt_server_restore_entry;

/** @brief Where the walk of a snapshot is. */
typedef enum {
    /** @brief No snapshot is under way. */
    MQTT_SERVER_SNAPSHOT_IDLE = 0,
    /** @brief The messages. */
    MQTT_SERVER_SNAPSHOT_MESSAGES,
    /** @brief The retained messages. */
    MQTT_SERVER_SNAPSHOT_RETAINED,
    /** @brief The Sessions, each with what it holds. */
    MQTT_SERVER_SNAPSHOT_SESSIONS,
    /** @brief The END record is next. */
    MQTT_SERVER_SNAPSHOT_END,
    /** @brief The END has been given. */
    MQTT_SERVER_SNAPSHOT_OVER
} mqtt_server_snapshot_phase;

/** @brief What a server keeps about its journal. */
typedef struct {
    /** @brief Where it stands. */
    mqtt_server_store_state state;
    /** @brief The generation of the journal segment being written. */
    uint64_t generation;
    /** @brief The base of that segment: the last group before it. */
    uint64_t segment_base;
    /** @brief The last group a COMMIT has closed. */
    uint64_t closed;
    /** @brief The last group the application has reported stored. */
    uint64_t stored;
    /** @brief The time the journal segment began. */
    uint64_t segment_start_ms;
    /** @brief The store id the next message or delivery is given. */
    uint64_t next_id;
    /** @brief Number of bytes written to the journal segment. */
    uint64_t journal_bytes;
    /** @brief The first connection on the list of those with answers held. */
    uint32_t held_head;
    /** @brief The most any record of the open group has to outlast. */
    mqtt_store_durability group_durability;
    /** @brief true if records have been written since the last COMMIT. */
    bool open;
    /**
     * @brief true if the snapshot of the current generation is whole, so that
     * the next one starts a new generation.
     */
    bool snapshot_whole;
    /**
     * @brief true while the journal segment follows a gap, so that no answer
     * goes until the snapshot of its generation is whole.
     */
    bool recovering;
    /** @brief Where the snapshot's walk is. */
    mqtt_server_snapshot_phase phase;
    /**
     * @brief The store ids the snapshot holds are below this: what was named
     * after it began is in the journal.
     */
    uint64_t walk_end;
    /** @brief Number of records the snapshot has given, END left out. */
    uint64_t snapshot_records;
    /** @brief The message, node or Session the walk is at. */
    uint32_t snapshot_index;
    /** @brief The subscription, delivery or flow of that Session. */
    uint32_t snapshot_item;
    /** @brief What of that Session is next. */
    uint32_t snapshot_step;
    /** @brief The runs of the record the snapshot last gave. */
    mqtt_run snapshot_runs[MQTT_SERVER_STORE_RUNS];
} mqtt_server_store;

/**
 * @brief A server. Treat it as opaque. It is here so that the caller can
 * provide the memory.
 */
typedef struct {
    /**
     * @brief MQTT_BOOL_SET once mqtt_server_init has succeeded. Every function
     * of the interface refuses a server that holds anything else.
     */
    mqtt_bool ready;
    /** @brief The configuration. */
    mqtt_server_config config;
    /** @brief The memory. */
    mqtt_server_storage storage;
    /** @brief What has been counted. */
    mqtt_server_statistics statistics;
    /** @brief The arena of variable-sized data. */
    mqtt_page_arena arena;
    /** @brief The blocks of the connections' rings. */
    mqtt_pool pool;
    /**
     * @brief The most bytes of a PUBLISH packet that go in a ring, three
     * quarters of a connection's hard quota. A larger Payload is sent straight
     * from the arena.
     */
    uint32_t message_room;
    /** @brief The largest packet kept in a receive ring. */
    uint32_t receive_inline;
    /** @brief The runs of a Payload received in blocks, being passed on. */
    mqtt_run receive_runs[MQTT_SERVER_RECEIVE_RUNS];
    /** @brief The topic tree. */
    mqtt_tree tree;
    /** @brief The time given in the last call. */
    uint64_t now_ms;
    /** @brief When the timers are next looked at. */
    uint64_t next_sweep_ms;
    /**
     * @brief The number of the next Assigned Client Identifier made without
     * random bytes.
     */
    uint64_t assigned_counter;
    /** @brief Number of Sessions in the session table. */
    uint32_t session_table_used;
    /** @brief Which connections are in use. */
    mqtt_slab connection_slots;
    /** @brief Which Sessions are in use. */
    mqtt_slab session_slots;
    /** @brief Which deliveries are in use. */
    mqtt_slab delivery_slots;
    /** @brief Which messages are in use. */
    mqtt_slab message_slots;
    /** @brief Which subscriptions are in use. */
    mqtt_slab subscription_slots;
    /** @brief Which shared groups are in use. */
    mqtt_slab group_slots;
    /** @brief The first connection with bytes to send. */
    uint32_t pending_head;
    /** @brief The last connection with bytes to send. */
    uint32_t pending_tail;
    /** @brief The connection that has waited longest for blocks. */
    uint32_t waiter_head;
    /** @brief The connection that has waited least long for blocks. */
    uint32_t waiter_tail;
    /**
     * @brief Space for the Assigned Client Identifier of the connection being
     * accepted.
     */
    uint8_t assigned_text[MQTT_SERVER_ASSIGNED_LENGTH];
    /** @brief The journal. */
    mqtt_server_store store;
} mqtt_server;

/**
 * @brief Fill a configuration with the defaults.
 * @param[out] config The configuration.
 */
void mqtt_server_config_default(mqtt_server_config *config);

/**
 * @brief Count the words of page metadata a server's storage needs: the
 * pages of every arena, each holding MQTT_PAGE_META_WORDS.
 * @param[in] storage The storage, whose counts and numbers of pages are set.
 * @return Number of words for page_meta, or 0xFFFFFFFF if no array could hold
 * them.
 */
uint32_t mqtt_server_meta_words(const mqtt_server_storage *storage);

/**
 * @brief Set up a server.
 *
 * Storage that mqtt_server_meta_words finds too large for any array is
 * refused, whatever page_meta_words says, and so are more Topic Aliases in
 * all than a 32-bit count holds.
 *
 * @param[out] server The server.
 * @param[in] config The configuration, which is copied.
 * @param[in] storage The memory, whose description is copied.
 * @return MQTT_OK or MQTT_INVALID_ARGUMENT.
 */
mqtt_status mqtt_server_init(mqtt_server *server,
                             const mqtt_server_config *config,
                             const mqtt_server_storage *storage);

/**
 * @brief Report a new Network Connection.
 * @param[in,out] server The server.
 * @param[in] now_ms The time.
 * @param[out] connection Its index.
 * @return MQTT_OK or MQTT_QUOTA_EXCEEDED.
 */
mqtt_status mqtt_server_connection_open(mqtt_server *server, uint64_t now_ms,
                                        uint32_t *connection);

/**
 * @brief Report that a Network Connection is gone. Its index can no longer be
 * used.
 * @param[in,out] server The server.
 * @param[in] connection Its index. One that is not open is ignored.
 * @param[in] now_ms The time.
 */
void mqtt_server_connection_closed(mqtt_server *server, uint32_t connection,
                                   uint64_t now_ms);

/**
 * @brief Find how many bytes a connection may be given now.
 *
 * Replies never take more bytes than the packets they answer, so reading no
 * more than this is what keeps there being room for replies. It is zero while
 * the Client is not reading what it is sent, or while the pool has too few
 * blocks free.
 *
 * @param[in] server The server.
 * @param[in] connection The connection.
 * @return The number of bytes, which is zero for a connection that is not open.
 */
uint32_t mqtt_server_receive_capacity(const mqtt_server *server,
                                      uint32_t connection);

/**
 * @brief Decide whether to read from a connection now.
 *
 * With no room, a connection that holds part of a packet whose size is known
 * moves it to the arena, so that its blocks go back to the pool. One with
 * nothing to send waits for blocks, and is marked as having work to do when
 * some come back: mqtt_server_next_pending hands it out, and asking again then
 * gives the answer.
 *
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @return true if it has room for bytes.
 */
bool mqtt_server_receive_wanted(mqtt_server *server, uint32_t connection);

/**
 * @brief Find where the next bytes a connection sends can be written, and take
 * the blocks for them.
 *
 * The places hold no more than mqtt_server_receive_capacity. Each call must be
 * followed by mqtt_server_receive_commit, with zero if nothing came, which
 * gives back the blocks nothing was written to.
 *
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[out] spaces The places, in the order bytes must fill them.
 * @param[in] max_spaces Number of entries spaces can hold.
 * @return Number of places, or zero if there is no room now.
 */
uint32_t mqtt_server_receive_space(mqtt_server *server, uint32_t connection,
                                   mqtt_space *spaces, uint32_t max_spaces);

/**
 * @brief Process bytes written to the places mqtt_server_receive_space gave.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] count Number of bytes written, in order. No more than the places
 * hold are counted.
 * @param[in] now_ms The time.
 * @return MQTT_OK, MQTT_INVALID_ARGUMENT for a connection that is not open, or
 * the reason the connection is now closing.
 */
mqtt_status mqtt_server_receive_commit(mqtt_server *server, uint32_t connection,
                                       uint32_t count, uint64_t now_ms);

/**
 * @brief Process bytes a connection has sent, copying them in through
 * mqtt_server_receive_space and mqtt_server_receive_commit.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes in data.
 * @param[in] now_ms The time.
 * @return MQTT_OK, MQTT_INVALID_ARGUMENT for a connection that is not open,
 * MQTT_RING_FULL if there was no room for all the bytes, when those left over
 * are not taken, or the reason the connection is now closing.
 */
mqtt_status mqtt_server_receive(mqtt_server *server, uint32_t connection,
                                const uint8_t *data, uint32_t length,
                                uint64_t now_ms);

/**
 * @brief Find the next bytes to send on a connection, as runs that one system
 * call can send together.
 *
 * The bytes are in one run for each block of the connection's ring, and one
 * for a Payload sent straight from the arena. Asking for fewer runs than that
 * gives the first ones.
 *
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[out] runs The runs, in the order they go. They stay valid until the
 * next call about this server.
 * @param[in] max_runs Number of entries runs can hold.
 * @return Number of runs, or zero if there is nothing to send now or the
 * connection is not open.
 */
uint32_t mqtt_server_transmit_view(mqtt_server *server, uint32_t connection,
                                   mqtt_run *runs, uint32_t max_runs);

/**
 * @brief Report how many bytes of that view have been sent, from its first
 * run on.
 * @param[in,out] server The server.
 * @param[in] connection The connection. One that is not open is ignored.
 * @param[in] count Number of bytes sent, which may end in any run. No more than
 * are waiting are taken.
 */
void mqtt_server_transmit_consume(mqtt_server *server, uint32_t connection,
                                  uint32_t count);

/**
 * @brief Check that every block of the pool is either free or in exactly one
 * ring of a connection in use, and that the counts agree.
 * @param[in] server The server.
 * @param[in,out] marks One byte for each block, all zero.
 * @return true if they do.
 */
bool mqtt_server_pool_audit(const mqtt_server *server, uint8_t *marks);

/**
 * @brief Find a connection that has work to do since the last call.
 * @param[in,out] server The server.
 * @return The connection, or MQTT_SERVER_NONE if there are no more.
 */
uint32_t mqtt_server_next_pending(mqtt_server *server);

/**
 * @brief Find out whether a connection should now be closed.
 * @param[in] server The server.
 * @param[in] connection The connection.
 * @return true if it is closing and has nothing left to send, and false if it
 * is not open.
 */
bool mqtt_server_should_close(const mqtt_server *server, uint32_t connection);

/**
 * @brief Find what a connection has sent since the last call, and start
 * again.
 *
 * A packet counts from its first byte, so one that has only partly arrived
 * counts too. A network layer uses this to tell a Client that only keeps its
 * connection alive from one with traffic.
 *
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @return What it has sent, or MQTT_SERVER_ACTIVITY_NONE if it is not open.
 */
mqtt_server_activity mqtt_server_take_activity(mqtt_server *server,
                                               uint32_t connection);

/**
 * @brief Find whether a connection's CONNECT has been accepted.
 * @param[in] server The server.
 * @param[in] connection The connection.
 * @return true once the CONNACK has accepted it, and until it is closing.
 */
bool mqtt_server_accepted(const mqtt_server *server, uint32_t connection);

/**
 * @brief Find the Keep Alive in force on a connection: the Client's, or the
 * Server Keep Alive that replaced it.
 * @param[in] server The server.
 * @param[in] connection The connection.
 * @return Seconds, or 0 for none or for a connection not accepted.
 */
uint16_t mqtt_server_keep_alive(const mqtt_server *server,
                                uint32_t connection);

/**
 * @brief Find whether a connection holds nothing: no part of a packet that has
 * arrived, and nothing to send.
 * @param[in] server The server.
 * @param[in] connection The connection.
 * @return true if it holds nothing, and false if it is not open.
 */
bool mqtt_server_idle(const mqtt_server *server, uint32_t connection);

/**
 * @brief Run the timers: keep alive, Will Delay Interval, Session Expiry
 * Interval.
 * @param[in,out] server The server.
 * @param[in] now_ms The time.
 * @return Number of milliseconds until the timers should next run.
 */
uint32_t mqtt_server_tick(mqtt_server *server, uint64_t now_ms);

/**
 * @brief Publish an Application Message from the Server's side, to every
 * Session whose subscriptions match its Topic Name.
 *
 * The Topic Name and the Payload are copied, so they can be reused when it
 * returns. With retain, a Payload of zero bytes removes the topic's retained
 * message, and is still sent to those subscribed, as one from a Client would
 * be.
 *
 * @param[in,out] server The server.
 * @param[in] topic_name The Topic Name.
 * @param[in] payload The Payload.
 * @param[in] qos The QoS, from 0 to 2.
 * @param[in] retain true to make it the topic's retained message.
 * @param[in] now_ms The time.
 * @return MQTT_OK; MQTT_TOPIC_NAME_INVALID; MQTT_MALFORMED_PACKET if the Topic
 * Name isn't valid UTF-8; MQTT_INVALID_ARGUMENT if the server isn't set up,
 * qos is above 2, or retain is true and retain_available is false; or
 * MQTT_QUOTA_EXCEEDED if the Topic Name doesn't fit in a connection's ring, or
 * there was no room to retain the message or to queue it for some Session. In
 * the last two cases the Sessions that had room have it, so publishing it
 * again sends them a second copy. A retained message that found no room still
 * replaces the old one, which is gone.
 */
mqtt_status mqtt_server_publish(mqtt_server *server,
                                const mqtt_utf8 *topic_name,
                                const mqtt_payload *payload, uint8_t qos,
                                bool retain, uint64_t now_ms);

/**
 * @brief Find the next retained message, for an application that wants them to
 * outlast a restart.
 *
 * The library touches no files, so saving them is up to the application. What
 * this hands back, and the cursor, are only good until the server next runs.
 *
 * @param[in] server The server.
 * @param[in] now_ms The time.
 * @param[in,out] cursor Where to look from. Set this to zero to start.
 * @param[out] message The message.
 * @return true if there was one, or false if there are no more.
 */
bool mqtt_server_retained_next(const mqtt_server *server, uint64_t now_ms,
                               uint32_t *cursor, mqtt_server_retained *message);

/**
 * @brief Put back a retained message that mqtt_server_retained_next handed out,
 * before any Client has connected.
 * @param[in,out] server The server.
 * @param[in] message The message.
 * @param[in] now_ms The time.
 * @return MQTT_OK; MQTT_TOPIC_NAME_INVALID; MQTT_MALFORMED_PACKET if the
 * Topic Name isn't well-formed UTF-8; MQTT_INVALID_ARGUMENT if the server
 * isn't set up, retain_available is false, the Payload is empty, the QoS is
 * above 2, or the Properties aren't ones a PUBLISH can carry, or hold a Topic
 * Alias, a Subscription Identifier or a Message Expiry Interval; or
 * MQTT_QUOTA_EXCEEDED.
 */
mqtt_status mqtt_server_retained_restore(mqtt_server *server,
                                         const mqtt_server_retained *message,
                                         uint64_t now_ms);

/**
 * @brief Close a connection from the Server's side, telling the Client why if
 * the protocol allows.
 * @param[in,out] server The server.
 * @param[in] connection The connection. One that is not open is ignored.
 * @param[in] reason_code The Reason Code.
 */
void mqtt_server_disconnect(mqtt_server *server, uint32_t connection,
                            uint8_t reason_code);

/**
 * @brief Start a journal segment, for the snapshot of a new generation.
 *
 * Nothing is written before the first call. Every record after it belongs to
 * the new segment, so the application starts that segment with the journal
 * header before it lets the server run again. Every group before it must have
 * been closed with mqtt_server_store_commit.
 *
 * A snapshot that was aborted is taken again in the same generation, which
 * goes on with the same segment: new_segment is then false, and the journal
 * header is not written. After a gap in the journal, the new segment is marked
 * as following one, and no answer goes until the snapshot is whole.
 *
 * @param[in,out] server The server.
 * @param[in] now_ms The time.
 * @param[out] snapshot_header Where the MQTT_STORE_HEADER_SIZE bytes of the
 * snapshot's header go.
 * @param[out] journal_header Where the MQTT_STORE_HEADER_SIZE bytes of the
 * journal segment's header go.
 * @param[out] new_segment true if a new journal segment starts.
 * @return MQTT_OK; MQTT_INVALID_ARGUMENT if the server isn't set up or keeps
 * nothing; or MQTT_INVALID_STATE if a group is still open or a snapshot is
 * under way.
 */
mqtt_status mqtt_server_snapshot_begin(mqtt_server *server, uint64_t now_ms,
                                       uint8_t *snapshot_header,
                                       uint8_t *journal_header,
                                       bool *new_segment);

/**
 * @brief Give the next record of the snapshot under way.
 *
 * The server may run between two calls, and the snapshot stays right, as each
 * record says what something is, and what changes meanwhile is in the
 * journal. The records of one Session come one after another, and if the
 * journal is written to before they are all given, that Session starts again:
 * a loop that has a budget for each round so stops where between is true.
 *
 * @param[in,out] server The server.
 * @param[out] space Where the record's fixed fields and a Topic Filter go,
 * valid until the next call.
 * @param[in] size Number of bytes in space, at least
 * MQTT_SERVER_SNAPSHOT_SPACE.
 * @param[out] record The record. Its runs are valid until the next call.
 * @param[out] between true if the next call starts a new message or Session.
 * @return MQTT_OK with a record; MQTT_NOT_FOUND once END has been given;
 * MQTT_BUFFER_TOO_SMALL if the record doesn't fit in space, when the snapshot
 * has to be aborted; or MQTT_INVALID_STATE if no snapshot is under way.
 */
mqtt_status mqtt_server_snapshot_next(mqtt_server *server, uint8_t *space,
                                      uint32_t size, mqtt_store_record *record,
                                      bool *between);

/**
 * @brief Report that the snapshot is whole in every copy, so that the next one
 * begins a new generation. After a gap in the journal, the answers that waited
 * for it go once their groups are stored.
 * @param[in,out] server The server.
 */
void mqtt_server_snapshot_done(mqtt_server *server);

/**
 * @brief Give up the snapshot under way. The next one is of the same
 * generation, and goes on with the same journal segment.
 * @param[in,out] server The server.
 */
void mqtt_server_snapshot_abort(mqtt_server *server);

/**
 * @brief Find whether a snapshot should begin: none has been whole in this
 * generation, the journal segment has grown past store_snapshot_bytes, or the
 * journal is broken.
 * @param[in] server The server.
 * @return true if one should, and none is under way.
 */
bool mqtt_server_snapshot_due(const mqtt_server *server);

/**
 * @brief Close the records written since the last call into a group, with a
 * COMMIT record.
 *
 * The answers that wait for them go once the application reports the group
 * stored with mqtt_server_stored. The COMMIT says, by its durability, whether
 * the group needs a sync first. A loop calls this once a round, after the
 * events, so that one sync covers everything that came in the round.
 *
 * @param[in,out] server The server.
 * @param[in] now_ms The time.
 * @param[out] sequence The last group closed, which is the one to report.
 * @return MQTT_OK, also when there was nothing to close;
 * MQTT_INVALID_ARGUMENT if the server isn't set up or keeps nothing; or
 * MQTT_INVALID_STATE if the journal is broken, when sequence is the last group
 * closed before it broke.
 */
mqtt_status mqtt_server_store_commit(mqtt_server *server, uint64_t now_ms,
                                     uint64_t *sequence);

/**
 * @brief Report that every group up to one is stored as its COMMIT asked, so
 * that the answers waiting for them can go.
 *
 * Each connection that can send again is handed out by
 * mqtt_server_next_pending.
 *
 * @param[in,out] server The server.
 * @param[in] sequence The last group stored. One above the last group closed
 * counts as that one.
 */
void mqtt_server_stored(mqtt_server *server, uint64_t sequence);

/**
 * @brief Report that records may have been lost, as when a sync fails.
 *
 * The journal is then broken. Nothing more is written to it, and every answer
 * waits, until a snapshot of a new generation is whole.
 *
 * @param[in,out] server The server.
 */
void mqtt_server_store_failed(mqtt_server *server);

/**
 * @brief Find whether records have been written that no COMMIT has closed yet.
 * A loop that finds some calls mqtt_server_store_commit before it waits.
 * @param[in] server The server.
 * @return true if there are some.
 */
bool mqtt_server_store_open(const mqtt_server *server);

/**
 * @brief Put back what a store holds, into a server just set up.
 *
 * Every copy of the snapshot and both journal places are read and checked
 * first. Then the newest copy that its journal segments follow on from is
 * applied, and those segments up to their last COMMIT. Each record is checked
 * as what a Client sends is, and it has to fit this configuration. Times come
 * back as what was left of them, less source->downtime_ms. Sessions come back
 * without their connections: a Session that had one when the store was last
 * written counts as having lost it then.
 *
 * It is all or nothing. If anything is wrong, the server is set up again,
 * empty, the fault says what and where, and the application sets the store
 * aside: a store from a later version, or one that doesn't fit this
 * configuration or this memory, can then be read by a server that can take
 * it. A store with nothing in it gives MQTT_OK and changes nothing. Nothing is
 * written until mqtt_server_snapshot_begin, which mqtt_server_snapshot_due
 * then asks for.
 *
 * @param[in,out] server The server, set up with a store and holding nothing.
 * @param[in] source The store, with from 1 to MQTT_SERVER_RESTORE_COPIES
 * copies.
 * @param[out] map Room for the table of store ids: storage message_count and
 * delivery_count entries always suffice.
 * @param[in] map_count Number of entries in map.
 * @param[out] buffer Room for the reads, of at least
 * MQTT_SERVER_RESTORE_MINIMUM bytes.
 * @param[in] size Number of bytes in buffer.
 * @param[in] now_ms The time.
 * @param[out] fault What was wrong, when the result is MQTT_STORE_DAMAGED.
 * @return MQTT_OK; MQTT_STORE_DAMAGED; MQTT_INVALID_ARGUMENT if the server
 * isn't set up, keeps nothing, or an argument is wrong; or MQTT_INVALID_STATE
 * if the server already holds something.
 */
mqtt_status mqtt_server_restore(mqtt_server *server,
                                const mqtt_store_source *source,
                                mqtt_server_restore_entry *map,
                                uint32_t map_count, uint8_t *buffer,
                                uint32_t size, uint64_t now_ms,
                                mqtt_store_fault *fault);

#endif /* MQTT_BASE_SERVER_H */
