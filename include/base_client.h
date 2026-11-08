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
 * @file base_client.h
 * @brief The MQTT Client, for MQTT 3.1, MQTT 3.1.1 and MQTT 5.0.
 *
 * Like the server, the client does no input or output of its own, and all its
 * memory is provided at start-up. The caller hands over the bytes that arrive
 * with mqtt_client_receive, and takes the bytes to send with
 * mqtt_client_transmit_view and mqtt_client_transmit_consume.
 *
 * The Session State survives from one network connection to the next, as long
 * as the same mqtt_client is used: QoS 1 and QoS 2 messages that were not
 * acknowledged are sent again when mqtt_client_connect resumes the Session.
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
#ifndef MQTT_BASE_CLIENT_H
#define MQTT_BASE_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "base_arena.h"
#include "base_bool.h"
#include "base_packet.h"
#include "base_pool.h"
#include "base_ring.h"
#include "base_status.h"
#include "base_store.h"
#include "base_utf8_t.h"

/** @brief The index that stands for nothing in the client's tables. */
#define MQTT_CLIENT_NONE 0xFFFFFFFFU
/**
 * @brief Number of bytes mqtt_client_snapshot_next needs: room for the frame
 * and the fixed fields of any record.
 */
#define MQTT_CLIENT_SNAPSHOT_SPACE 32U
/**
 * @brief Number of bytes mqtt_client_restore needs at least. Packets and the
 * Client Identifier are read straight into the arena, so this is for the rest.
 */
#define MQTT_CLIENT_RESTORE_MINIMUM 256U
/** @brief The most snapshot copies mqtt_client_restore reads. */
#define MQTT_CLIENT_RESTORE_COPIES 8U

/** @brief The state of a client. */
typedef enum {
    /** @brief No network connection. */
    MQTT_CLIENT_OFFLINE = 0,
    /**
     * @brief The CONNECT packet has been sent, and the CONNACK packet has not
     * arrived.
     */
    MQTT_CLIENT_CONNECTING,
    /** @brief The connection has been accepted. */
    MQTT_CLIENT_ACTIVE,
    /**
     * @brief The connection is to be closed once the bytes waiting have been
     * sent.
     */
    MQTT_CLIENT_CLOSING
} mqtt_client_state;

/** @brief The stage of a message on its way to the Server. */
typedef enum {
    /** @brief Waiting to be sent. */
    MQTT_CLIENT_MESSAGE_QUEUED = 0,
    /** @brief Sent, and waiting for the PUBACK or PUBREC packet. */
    MQTT_CLIENT_MESSAGE_SENT,
    /** @brief PUBREL sent, and waiting for the PUBCOMP packet. */
    MQTT_CLIENT_MESSAGE_RELEASED
} mqtt_client_message_state;

/** @brief A PUBLISH packet held until it has been acknowledged. */
typedef struct {
    /** @brief The next entry in the list. */
    uint32_t next;
    /** @brief The previous entry in the list. */
    uint32_t previous;
    /**
     * @brief Arena block with the whole packet, or MQTT_ARENA_NONE once it is
     * not needed.
     */
    uint32_t block;
    /** @brief Number of bytes in the packet. */
    uint32_t size;
    /** @brief The stage. */
    mqtt_client_message_state state;
    /**
     * @brief The Packet Identifier, or zero for a QoS 0 message waiting for its
     * turn.
     */
    uint16_t packet_identifier;
    /** @brief The Topic Alias in the packet, or zero. */
    uint16_t topic_alias;
    /** @brief The QoS of the packet. */
    uint8_t qos;
    /** @brief The RETAIN flag of the packet. */
    bool retain;
    /** @brief true if its PUBLISH packet was sent on the current connection. */
    bool on_wire;
} mqtt_client_message;

/** @brief A SUBSCRIBE or UNSUBSCRIBE packet that has not been answered. */
typedef struct {
    /** @brief The Packet Identifier, or zero if the entry is free. */
    uint16_t packet_identifier;
    /**
     * @brief With MQTT 3.1, an arena block with the packet to send again, or
     * MQTT_ARENA_NONE.
     */
    uint32_t block;
    /** @brief Number of bytes of the packet in the block. */
    uint32_t length;
} mqtt_client_pending;

/** @brief A Topic Alias the Server has set. */
typedef struct {
    /** @brief Arena block with the Topic Name, or MQTT_ARENA_NONE. */
    uint32_t block;
    /** @brief Number of bytes in the Topic Name. */
    uint16_t length;
} mqtt_client_alias;

/** @brief What has happened, for the event callback. */
typedef enum {
    /** @brief A CONNACK packet: connack is set. */
    MQTT_CLIENT_EVENT_CONNACK = 0,
    /** @brief An AUTH packet: reason is set. */
    MQTT_CLIENT_EVENT_AUTH,
    /** @brief An Application Message: publish is set. */
    MQTT_CLIENT_EVENT_MESSAGE,
    /**
     * @brief A message of this Client's has finished its flow: response is set.
     */
    MQTT_CLIENT_EVENT_PUBLISHED,
    /** @brief A SUBACK packet: subscribed is set. */
    MQTT_CLIENT_EVENT_SUBACK,
    /** @brief An UNSUBACK packet: subscribed is set. */
    MQTT_CLIENT_EVENT_UNSUBACK,
    /** @brief A PINGRESP packet. */
    MQTT_CLIENT_EVENT_PINGRESP,
    /** @brief A DISCONNECT packet from the Server: reason is set. */
    MQTT_CLIENT_EVENT_DISCONNECT
} mqtt_client_event_type;

/**
 * @brief An event. The pointers and what they refer to are only valid during
 * the callback.
 */
typedef struct {
    /** @brief What has happened. */
    mqtt_client_event_type type;
    /** @brief The CONNACK packet. */
    const mqtt_connack *connack;
    /** @brief The Application Message. */
    const mqtt_publish *publish;
    /** @brief The acknowledgement that ended a message's flow. */
    const mqtt_publish_response *response;
    /** @brief The SUBACK or UNSUBACK packet. */
    const mqtt_subscribe_response *subscribed;
    /** @brief The DISCONNECT or AUTH packet. */
    const mqtt_reason *reason;
} mqtt_client_event;

/**
 * @brief Be told what has happened.
 * @param[in] user_tag The tag from the storage.
 * @param[in] event The event.
 */
typedef void (*mqtt_client_event_function)(uint32_t user_tag,
                                           const mqtt_client_event *event);

/** @brief All the memory a client uses, and where its events go. */
typedef struct {
    /** @brief Gets the events, or NULL to ignore them. */
    mqtt_client_event_function on_event;
    /** @brief Hands to the callback. */
    uint32_t user_tag;
    /**
     * @brief Holds a packet that has arrived in parts, which limits the largest
     * packet.
     */
    uint8_t *receive_bytes;
    /** @brief Number of bytes in receive_bytes. */
    uint32_t receive_size;
    /**
     * @brief Bytes of the blocks of the transmit ring: pool_block_count times
     * pool_block_size.
     */
    uint8_t *pool_bytes;
    /** @brief One link for each block. */
    uint32_t *pool_next;
    /** @brief Number of bytes in a block, at most MQTT_POOL_BLOCK_MAX. */
    uint32_t pool_block_size;
    /** @brief Number of blocks. */
    uint32_t pool_block_count;
    /** @brief Messages held until they are acknowledged. */
    mqtt_client_message *messages;
    /** @brief Number of messages. */
    uint32_t message_count;
    /**
     * @brief Packet Identifiers of QoS 2 messages from the Server, between
     * PUBLISH and PUBREL.
     */
    uint16_t *incoming;
    /**
     * @brief Number of entries, which is the Receive Maximum this Client can
     * honour.
     */
    uint32_t incoming_count;
    /** @brief SUBSCRIBE and UNSUBSCRIBE packets waiting for an answer. */
    mqtt_client_pending *pending;
    /** @brief Number of entries. */
    uint32_t pending_count;
    /** @brief Topic Aliases the Server may set. */
    mqtt_client_alias *aliases;
    /**
     * @brief Number of entries, which is the Topic Alias Maximum this Client
     * can honour.
     */
    uint32_t alias_count;
    /**
     * @brief Bytes for held packets and Topic Names: arena_units times
     * MQTT_ARENA_UNIT.
     */
    uint8_t *arena_bytes;
    /** @brief One state byte for each unit. */
    uint8_t *arena_states;
    /** @brief Number of units. */
    uint32_t arena_units;
} mqtt_client_storage;

/** @brief How a client keeps its Session State in a store. */
typedef struct {
    /** @brief Hands each record of the journal to the application. */
    mqtt_store_append append;
    /** @brief The value given to append. */
    uint32_t user_tag;
    /**
     * @brief What records about the Session have to outlast: its version,
     * whether it exists, and the Client Identifier. MQTT_STORE_MEMORY writes
     * nothing at all.
     */
    mqtt_store_durability sessions;
    /**
     * @brief What records about messages have to outlast: those held for the
     * Server, the QoS 2 Packet Identifiers received, and with MQTT 3.1 the
     * SUBSCRIBE and UNSUBSCRIBE packets waiting for an answer.
     * MQTT_STORE_MEMORY writes none, so a restart keeps the Session without
     * its messages.
     */
    mqtt_store_durability messages;
    /**
     * @brief Number of bytes of journal segment after which a snapshot is due,
     * so that the journal starts again.
     */
    uint64_t snapshot_bytes;
} mqtt_client_store_config;

/** @brief Where a client's journal stands. */
typedef enum {
    /** @brief Nothing is written: there is no store, or it keeps nothing. */
    MQTT_CLIENT_STORE_OFF = 0,
    /** @brief Nothing is written until the first snapshot begins. */
    MQTT_CLIENT_STORE_FRESH,
    /** @brief Each change is written to the journal. */
    MQTT_CLIENT_STORE_RUNNING,
    /**
     * @brief A record was refused, or a sync failed. Nothing more is written,
     * and every answer waits, until a snapshot is whole.
     */
    MQTT_CLIENT_STORE_BROKEN
} mqtt_client_store_state;

/** @brief Where the walk of a client's snapshot is. */
typedef enum {
    /** @brief No snapshot is under way. */
    MQTT_CLIENT_SNAPSHOT_IDLE = 0,
    /** @brief The Session. */
    MQTT_CLIENT_SNAPSHOT_SESSION,
    /** @brief The messages held for the Server. */
    MQTT_CLIENT_SNAPSHOT_MESSAGES,
    /** @brief The QoS 2 Packet Identifiers received. */
    MQTT_CLIENT_SNAPSHOT_INCOMING,
    /** @brief The SUBSCRIBE and UNSUBSCRIBE packets waiting, with MQTT 3.1. */
    MQTT_CLIENT_SNAPSHOT_PENDING,
    /** @brief The END record is next. */
    MQTT_CLIENT_SNAPSHOT_END,
    /** @brief The END has been given. */
    MQTT_CLIENT_SNAPSHOT_OVER
} mqtt_client_snapshot_phase;

/** @brief What a client keeps about its journal. */
typedef struct {
    /** @brief The configuration. */
    mqtt_client_store_config config;
    /** @brief Where it stands. */
    mqtt_client_store_state state;
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
    /** @brief Number of bytes written to the journal segment. */
    uint64_t journal_bytes;
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
    mqtt_client_snapshot_phase phase;
    /**
     * @brief true if the journal was written to while a snapshot is under way,
     * which the snapshot then no longer matches.
     */
    bool snapshot_changed;
    /** @brief The message, or the index of the entry, the walk is at. */
    uint32_t snapshot_item;
    /** @brief Number of records the snapshot has given, END left out. */
    uint64_t snapshot_records;
    /** @brief The runs of the record the snapshot last gave. */
    mqtt_run snapshot_runs[MQTT_STORE_RECORD_RUNS];
    /**
     * @brief Number of bytes of the ring that may go before the answers that
     * wait for the store, or MQTT_CLIENT_NONE if none waits.
     */
    uint32_t hold_bytes;
    /** @brief The group that has to be stored before they go. */
    uint64_t hold_sequence;
    /**
     * @brief true if the packet sent straight from the arena waits too, as it
     * was the first to.
     */
    bool hold_direct;
    /**
     * @brief Arena block with the Client Identifier of the Session, or
     * MQTT_ARENA_NONE if it isn't known.
     */
    uint32_t identifier_block;
    /** @brief Number of bytes of the Client Identifier. */
    uint16_t identifier_length;
    /** @brief true if the Client Identifier has changed since the last. */
    bool session_due;
    /** @brief The version the last CLIENT_SESSION record gave. */
    mqtt_version written_version;
    /** @brief Whether the last CLIENT_SESSION record said a Session exists. */
    bool written_exists;
} mqtt_client_store;

/**
 * @brief A client. Treat it as opaque. It is here so that the caller can
 * provide the memory.
 */
typedef struct {
    /**
     * @brief MQTT_BOOL_SET once mqtt_client_init has succeeded. Every function
     * of the interface refuses a client that holds anything else.
     */
    mqtt_bool ready;
    /** @brief The memory. */
    mqtt_client_storage storage;
    /** @brief The arena. */
    mqtt_arena arena;
    /** @brief The blocks of the transmit ring. */
    mqtt_pool pool;
    /** @brief Whole packets waiting to be sent. */
    mqtt_ring ring;
    /** @brief The state. */
    mqtt_client_state state;
    /** @brief The protocol in use. */
    mqtt_version version;
    /** @brief The time given in the last call. */
    uint64_t now_ms;
    /** @brief The time of the last packet queued for sending. */
    uint64_t last_sent_ms;
    /** @brief The time the CONNECT packet was sent. */
    uint64_t connect_sent_ms;
    /**
     * @brief The time a PINGREQ packet went unanswered from, or zero if none is
     * outstanding.
     */
    uint64_t ping_sent_ms;
    /** @brief The Keep Alive in force, in milliseconds. */
    uint64_t keep_alive_ms;
    /** @brief The first message held. */
    uint32_t list_head;
    /** @brief The last message held. */
    uint32_t list_tail;
    /** @brief The next message to send for the first time, or to send again. */
    uint32_t send_cursor;
    /** @brief The first free message. */
    uint32_t free_message;
    /** @brief The number of QoS 1 and QoS 2 messages on their way. */
    uint32_t on_wire;
    /** @brief The Server's Receive Maximum. */
    uint32_t send_quota;
    /** @brief The Server's Maximum Packet Size. */
    uint32_t server_maximum_packet_size;
    /** @brief Number of bytes in receive_bytes. */
    uint32_t receive_used;
    /**
     * @brief Number of bytes in the packet being received, or zero if not known
     * yet.
     */
    uint32_t receive_expected;
    /** @brief Number of entries used in the incoming table. */
    uint32_t incoming_used;
    /**
     * @brief The number of bytes in the ring that go before the direct bytes.
     */
    uint32_t direct_ring_bytes;
    /**
     * @brief The number of bytes at the front of the ring, up to the end of the
     * last CONNECT or AUTH packet queued, which are wiped as they go: they may
     * hold the Password or Authentication Data.
     */
    uint32_t secret_left;
    /** @brief The rest of a packet being sent directly from the arena. */
    const uint8_t *direct_data;
    /** @brief Number of bytes in direct_data. */
    uint32_t direct_left;
    /**
     * @brief A block to give back when direct_data has been sent, or
     * MQTT_ARENA_NONE.
     */
    uint32_t direct_free_block;
    /** @brief The block direct_data points into, or MQTT_ARENA_NONE. */
    uint32_t direct_block;
    /**
     * @brief The next message whose PUBREL to send again on this connection.
     */
    uint32_t release_cursor;
    /**
     * @brief Index of the next SUBSCRIBE or UNSUBSCRIBE to send again after
     * resuming.
     */
    uint32_t pending_cursor;
    /** @brief The last Packet Identifier used. */
    uint16_t last_packet_identifier;
    /** @brief The Server's Topic Alias Maximum. */
    uint16_t server_topic_alias_maximum;
    /** @brief The Server's Maximum QoS. */
    uint8_t server_maximum_qos;
    /** @brief The Server's Retain Available. */
    bool server_retain_available;
    /** @brief true if this Client knows of a Session it can resume. */
    bool session_exists;
    /**
     * @brief true once a connection has said which version held messages are
     * encoded for.
     */
    bool version_known;
    /** @brief true if the CONNECT packet had an Authentication Method. */
    bool authentication_method_sent;
    /**
     * @brief Arena block with that Authentication Method, or MQTT_ARENA_NONE.
     */
    uint32_t method_block;
    /** @brief Number of bytes in that Authentication Method. */
    uint16_t method_length;
    /**
     * @brief true if the CONNECT packet asked for no Reason Strings or User
     * Properties.
     */
    bool problem_information_off;
    /** @brief The Topic Alias Maximum the CONNECT packet gave the Server. */
    uint32_t alias_limit;
    /** @brief The Receive Maximum the CONNECT packet gave the Server. */
    uint32_t receive_limit;
    /** @brief The Maximum Packet Size the CONNECT packet gave the Server. */
    uint32_t packet_limit;
    /** @brief The time the connection started to close. */
    uint64_t closing_ms;
    /**
     * @brief Packets saying why this Client is closing that there was no room
     * to send.
     */
    uint32_t replies_lost;
    /** @brief The journal of what outlasts a restart. */
    mqtt_client_store store;
} mqtt_client;

/** @brief One Topic Filter to subscribe to. */
typedef struct {
    /** @brief The Topic Filter. */
    mqtt_utf8 topic_filter;
    /**
     * @brief The Subscription Options, or the Requested QoS before MQTT 5.0.
     */
    uint8_t options;
} mqtt_client_subscription;

/**
 * @brief Set up a client.
 * @param[out] client The client.
 * @param[in] storage The memory, whose description is copied.
 * @return MQTT_OK or MQTT_INVALID_ARGUMENT.
 */
mqtt_status mqtt_client_init(mqtt_client *client,
                             const mqtt_client_storage *storage);

/**
 * @brief Say that this Client has a Session from an earlier run, with nothing
 * in flight.
 *
 * Without this, a Session Present of 1 from the Server closes the connection,
 * as a Client with no Session State must do: [MQTT-3.2.2-4]. A program that
 * starts again and wants to carry on with the subscriptions it made before has
 * that state, in the sense of knowing what it subscribed to.
 *
 * @param[in,out] client The client.
 */
void mqtt_client_assume_session(mqtt_client *client);

/**
 * @brief Queue a CONNECT packet on a new network connection.
 *
 * Unless the packet sets them, this adds the Receive Maximum, Maximum Packet
 * Size and Topic Alias Maximum that the storage can honour.
 *
 * @param[in,out] client The client.
 * @param[in] packet The packet. The bytes it refers to are not used after this
 * call.
 * @param[in] now_ms The time.
 * @return MQTT_OK, MQTT_INVALID_STATE, or MQTT_BUFFER_TOO_SMALL.
 */
mqtt_status mqtt_client_connect(mqtt_client *client, const mqtt_connect *packet,
                                uint64_t now_ms);

/**
 * @brief Report that the network connection has gone.
 * @param[in,out] client The client.
 * @param[in] now_ms The time.
 */
void mqtt_client_connection_lost(mqtt_client *client, uint64_t now_ms);

/**
 * @brief Find how many bytes the client may be given now.
 * @param[in] client The client.
 * @return The number of bytes.
 */
uint32_t mqtt_client_receive_capacity(const mqtt_client *client);

/**
 * @brief Find where the next bytes from the Server can be written: the rest of
 * the receive buffer, as far as there is room for the replies.
 * @param[in] client The client.
 * @param[out] space The place.
 * @return Number of bytes it holds, zero if there is no room now.
 */
uint32_t mqtt_client_receive_space(const mqtt_client *client,
                                   mqtt_space *space);

/**
 * @brief Process bytes written to the place mqtt_client_receive_space gave.
 *
 * Each whole packet is handled where it is, and the start of one that has
 * only partly arrived is moved to the front of the buffer.
 *
 * @param[in,out] client The client.
 * @param[in] count Number of bytes written. No more than the place holds are
 * counted.
 * @param[in] now_ms The time.
 * @return MQTT_OK, or the reason the connection is now closing.
 */
mqtt_status mqtt_client_receive_commit(mqtt_client *client, uint32_t count,
                                       uint64_t now_ms);

/**
 * @brief Process bytes that arrived.
 * @param[in,out] client The client.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes in data.
 * @param[in] now_ms The time.
 * @return MQTT_OK, or the reason the connection is now closing.
 */
mqtt_status mqtt_client_receive(mqtt_client *client, const uint8_t *data,
                                uint32_t length, uint64_t now_ms);

/**
 * @brief Find the next bytes to send, as runs that one system call can send
 * together. Asking for one run gives the first one alone.
 * @param[in,out] client The client.
 * @param[out] runs The runs, in the order they go, valid until the next call
 * about this client.
 * @param[in] max_runs Number of entries runs can hold. One for each block of
 * the ring, and one for a message sent straight from the arena, is enough for
 * everything that is waiting.
 * @return Number of runs, or zero if there is nothing to send now.
 */
uint32_t mqtt_client_transmit_view(mqtt_client *client, mqtt_run *runs,
                                   uint32_t max_runs);

/**
 * @brief Report how many bytes of that view have been sent, from its first
 * run on.
 * @param[in,out] client The client.
 * @param[in] count Number of bytes sent, which may end in any run. No more than
 * are waiting are taken.
 */
void mqtt_client_transmit_consume(mqtt_client *client, uint32_t count);

/**
 * @brief Find out whether the connection should now be closed.
 * @param[in] client The client.
 * @return true if it is closing and has nothing left to send.
 */
bool mqtt_client_should_close(const mqtt_client *client);

/**
 * @brief Run the timers: Keep Alive, and the wait for a CONNACK packet.
 * @param[in,out] client The client.
 * @param[in] now_ms The time.
 * @return Number of milliseconds until the timers should next run.
 */
uint32_t mqtt_client_tick(mqtt_client *client, uint64_t now_ms);

/**
 * @brief Send an Application Message.
 *
 * Its QoS, Topic Name and Properties are checked here. A message at QoS 1 or
 * QoS 2 is copied and held until its flow ends, and may be given while offline,
 * once mqtt_client_connect has been called at least once so that its protocol
 * version is known. It then goes out with the next connection, unless that
 * Server's limits rule it out, in which case MQTT_CLIENT_EVENT_PUBLISHED
 * reports it with the Reason Code of the limit.
 *
 * A message at QoS 1 or QoS 2 must have a Topic Name, even with a Topic Alias.
 * It may be sent again on another connection, and a Topic Alias does not carry
 * over to one.
 *
 * @param[in,out] client The client.
 * @param[in,out] packet The packet. Its Packet Identifier is set here.
 * @param[in] now_ms The time.
 * @return MQTT_OK, or why it was not accepted, in which case nothing has been
 * kept.
 */
mqtt_status mqtt_client_publish(mqtt_client *client, mqtt_publish *packet,
                                uint64_t now_ms);

/**
 * @brief Queue a SUBSCRIBE packet.
 * @param[in,out] client The client.
 * @param[in] entries The Topic Filters.
 * @param[in] count Number of entries.
 * @param[in] properties The Properties, or NULL for none.
 * @param[out] packet_identifier The Packet Identifier the answer will have.
 * @return MQTT_OK, or why it was not accepted.
 */
mqtt_status mqtt_client_subscribe(mqtt_client *client,
                                  const mqtt_client_subscription *entries,
                                  uint32_t count,
                                  const mqtt5_properties *properties,
                                  uint16_t *packet_identifier);

/**
 * @brief Queue an UNSUBSCRIBE packet.
 * @param[in,out] client The client.
 * @param[in] filters The Topic Filters.
 * @param[in] count Number of entries.
 * @param[in] properties The Properties, or NULL for none.
 * @param[out] packet_identifier The Packet Identifier the answer will have.
 * @return MQTT_OK, or why it was not accepted.
 */
mqtt_status mqtt_client_unsubscribe(mqtt_client *client,
                                    const mqtt_utf8 *filters, uint32_t count,
                                    const mqtt5_properties *properties,
                                    uint16_t *packet_identifier);

/**
 * @brief Queue a DISCONNECT packet, and start to close.
 * @param[in,out] client The client.
 * @param[in] packet The packet, or NULL for a normal disconnection.
 * @return MQTT_OK, or why it could not be queued, in which case it closes
 * anyway.
 */
mqtt_status mqtt_client_disconnect(mqtt_client *client,
                                   const mqtt_reason *packet);

/**
 * @brief Queue an AUTH packet.
 * @param[in,out] client The client.
 * @param[in] packet The packet.
 * @return MQTT_OK, or why it could not be queued.
 */
mqtt_status mqtt_client_auth(mqtt_client *client, const mqtt_reason *packet);

/**
 * @brief Keep a client's Session State in a store, from now on.
 *
 * Call it after mqtt_client_init, before anything is connected or published,
 * and then mqtt_client_restore if the store may hold something. Nothing is
 * written until the first snapshot begins, which mqtt_client_snapshot_due
 * asks for, and which has to be whole before the first connection.
 *
 * Each change to what is held is then written as a record, and an answer
 * that tells the Server something was kept waits until the application
 * reports its group stored: PUBACK, PUBREC, PUBREL, PUBCOMP, a QoS 2 PUBLISH,
 * CONNECT, and with MQTT 3.1 SUBSCRIBE and UNSUBSCRIBE. A message from the
 * Server reaches the event callback before the answer to it is sent, so an
 * application that keeps what it is given there loses none of it.
 *
 * @param[in,out] client The client.
 * @param[in] config The configuration, which is copied.
 * @return MQTT_OK; MQTT_INVALID_ARGUMENT if the client isn't set up or the
 * configuration is wrong; or MQTT_INVALID_STATE if the client has already
 * been used, or keeps a store already.
 */
mqtt_status mqtt_client_store_init(mqtt_client *client,
                                   const mqtt_client_store_config *config);

/**
 * @brief Close the records written since the last call into a group, with a
 * COMMIT record.
 *
 * The answers that wait for them go once the application reports the group
 * stored with mqtt_client_stored. A loop calls this once a round, after the
 * bytes that arrived, so that one sync covers everything they caused.
 *
 * @param[in,out] client The client.
 * @param[in] now_ms The time.
 * @param[out] sequence The last group closed, which is the one to report.
 * @return MQTT_OK, also when there was nothing to close;
 * MQTT_INVALID_ARGUMENT if the client isn't set up or keeps nothing; or
 * MQTT_INVALID_STATE if the journal is broken.
 */
mqtt_status mqtt_client_store_commit(mqtt_client *client, uint64_t now_ms,
                                     uint64_t *sequence);

/**
 * @brief Report that every group up to one is stored as its COMMIT asked, so
 * that the answers waiting for them can go.
 * @param[in,out] client The client.
 * @param[in] sequence The last group stored.
 */
void mqtt_client_stored(mqtt_client *client, uint64_t sequence);

/**
 * @brief Report that records may have been lost, as when a sync fails. The
 * journal is then broken: nothing more is written, and every answer waits,
 * until a snapshot of a new generation is whole.
 * @param[in,out] client The client.
 */
void mqtt_client_store_failed(mqtt_client *client);

/**
 * @brief Find whether records have been written that no COMMIT has closed yet.
 * A loop that finds some calls mqtt_client_store_commit before it waits.
 * @param[in] client The client.
 * @return true if there are some.
 */
bool mqtt_client_store_open(const mqtt_client *client);

/**
 * @brief Find whether a snapshot should begin: none has been whole in this
 * generation, the journal segment has grown past snapshot_bytes, or the
 * journal is broken.
 * @param[in] client The client.
 * @return true if one should, and none is under way.
 */
bool mqtt_client_snapshot_due(const mqtt_client *client);

/**
 * @brief Begin a snapshot, which is taken whole: the client must not be called
 * otherwise until mqtt_client_snapshot_done or mqtt_client_snapshot_abort.
 *
 * The snapshot of a new generation starts a new journal segment, which the
 * application starts with the journal header once the first copy of the
 * snapshot is whole, and before mqtt_client_snapshot_done. Every group before
 * it must have been closed with mqtt_client_store_commit.
 *
 * @param[in,out] client The client.
 * @param[in] now_ms The time.
 * @param[out] snapshot_header Where the MQTT_STORE_HEADER_SIZE bytes of the
 * snapshot's header go.
 * @param[out] journal_header Where the MQTT_STORE_HEADER_SIZE bytes of the
 * journal segment's header go.
 * @param[out] new_segment true if a new journal segment starts.
 * @return MQTT_OK; MQTT_INVALID_ARGUMENT if the client isn't set up or keeps
 * nothing; or MQTT_INVALID_STATE if a group is still open or a snapshot is
 * under way.
 */
mqtt_status mqtt_client_snapshot_begin(mqtt_client *client, uint64_t now_ms,
                                       uint8_t *snapshot_header,
                                       uint8_t *journal_header,
                                       bool *new_segment);

/**
 * @brief Give the next record of the snapshot under way.
 * @param[in,out] client The client.
 * @param[out] space Where the record's frame and fixed fields go, valid until
 * the next call.
 * @param[in] size Number of bytes in space, at least
 * MQTT_CLIENT_SNAPSHOT_SPACE.
 * @param[out] record The record. Its runs are valid until the next call.
 * @return MQTT_OK with a record; MQTT_NOT_FOUND once END has been given;
 * MQTT_BUFFER_TOO_SMALL if space is too small; or MQTT_INVALID_STATE if no
 * snapshot is under way, or the client has changed since it began.
 */
mqtt_status mqtt_client_snapshot_next(mqtt_client *client, uint8_t *space,
                                      uint32_t size,
                                      mqtt_store_record *record);

/**
 * @brief Report that the snapshot's first copy is whole, and its journal
 * segment started if it starts one, so that the next one begins a new
 * generation. After a gap in the journal, the answers that waited for it go
 * once their group is stored.
 * @param[in,out] client The client.
 */
void mqtt_client_snapshot_done(mqtt_client *client);

/**
 * @brief Give up the snapshot under way.
 * @param[in,out] client The client.
 */
void mqtt_client_snapshot_abort(mqtt_client *client);

/**
 * @brief Put back what a store holds, into a client just given its store.
 *
 * Every place is read and checked first, then the newest snapshot copy that
 * its journal segments follow on from is applied, and those segments up to
 * their last COMMIT. Each record is checked as what the Server sends is, and
 * has to fit this client's memory. The client is then offline, with the
 * Session it had: held messages go out on the next connection that resumes
 * it, a message that was sent with DUP set.
 *
 * It is all or nothing. If anything is wrong, the client is set up again,
 * empty, with its store, the fault says what and where, and the application
 * sets the store aside. A store with nothing in it gives MQTT_OK and changes
 * nothing.
 *
 * @param[in,out] client The client, with a store and holding nothing.
 * @param[in] source The store, with from 1 to MQTT_CLIENT_RESTORE_COPIES
 * copies.
 * @param[out] buffer Room for the reads, of at least
 * MQTT_CLIENT_RESTORE_MINIMUM bytes.
 * @param[in] size Number of bytes in buffer.
 * @param[out] fault What was wrong, when the result is MQTT_STORE_DAMAGED.
 * @return MQTT_OK; MQTT_STORE_DAMAGED; MQTT_INVALID_ARGUMENT if the client
 * isn't set up, keeps nothing, or an argument is wrong; or MQTT_INVALID_STATE
 * if the client already holds something.
 */
mqtt_status mqtt_client_restore(mqtt_client *client,
                                const mqtt_store_source *source,
                                uint8_t *buffer, uint32_t size,
                                mqtt_store_fault *fault);

/**
 * @brief Find the Client Identifier of the Session the store holds: the one
 * the last CONNECT packet gave, or the one the Server assigned.
 * @param[in] client The client.
 * @param[out] identifier The identifier, valid until the next connection.
 * @return MQTT_OK, or MQTT_NOT_FOUND if none is known.
 */
mqtt_status mqtt_client_store_identifier(const mqtt_client *client,
                                         mqtt_utf8 *identifier);

#endif /* MQTT_BASE_CLIENT_H */
