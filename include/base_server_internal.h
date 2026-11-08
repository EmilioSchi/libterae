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
 * @file base_server_internal.h
 * @brief What the source files of the server share. It is not part of the
 * library's interface.
 *
 * base_server.c reads packets and decides what they mean.
 * base_server_connect.c handles CONNECT and AUTH packets, and accepts
 * connections. base_server_publish.c handles PUBLISH packets and the packets
 * that acknowledge them. base_server_session.c looks after Sessions,
 * subscriptions and retained messages. base_server_route.c stores messages,
 * finds where they go and turns them into bytes. base_server_pool.c decides how
 * many blocks of the pool each connection may take, and wakes the connections
 * waiting for them. base_server_receive.c takes in the bytes each connection
 * sends and cuts them into packets, base_server_transmit.c hands out the bytes
 * to send, and base_server_wait.c tells the network layer how busy each
 * connection is.
 */
#ifndef MQTT_BASE_SERVER_INTERNAL_H
#define MQTT_BASE_SERVER_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "base_binary_t.h"
#include "base_packet.h"
#include "base_payload_t.h"
#include "base_run_t.h"
#include "base_server.h"
#include "base_store_internal.h"
#include "base_utf8_t.h"
#include "base_version.h"

/** @brief A restore under way. */
typedef struct {
    /** @brief The server. */
    mqtt_server *server;
    /** @brief The store. */
    const mqtt_store_source *source;
    /** @brief The table of store ids. */
    mqtt_server_restore_entry *map;
    /** @brief Number of entries map holds. */
    uint32_t map_count;
    /** @brief Number of entries in use. */
    uint32_t map_used;
    /**
     * @brief Number of entries from the first that are in order of id. The
     * rest were added by the snapshot, and are put in order when it ends.
     */
    uint32_t map_sorted;
    /** @brief Room for the reads. */
    uint8_t *buffer;
    /** @brief Number of bytes in buffer. */
    uint32_t size;
    /** @brief Where a fault is described. */
    mqtt_store_fault *fault;
    /** @brief The part of the store being applied. */
    mqtt_store_part part;
    /** @brief The copy or journal place being applied. */
    uint32_t place;
    /** @brief The generation of that place. */
    uint64_t generation;
    /** @brief Offset of the record being applied. */
    uint64_t offset;
    /** @brief Kind of the record being applied. */
    uint8_t kind;
    /**
     * @brief The time of the record being applied, in milliseconds on one line
     * through every segment.
     */
    uint64_t clock;
    /** @brief Where the line stood when the segment being applied began. */
    uint64_t segment_offset;
    /** @brief The store id the server gives next, at least. */
    uint64_t next_id;
    /** @brief true while the snapshot is applied, false for the journal. */
    bool snapshot;
    /** @brief true once the messages of the snapshot are in order in map. */
    bool messages_sorted;
    /** @brief The Session the snapshot last began, or MQTT_SERVER_NONE. */
    uint32_t mark_session;
    /** @brief map_used when it began. */
    uint32_t mark_map;
    /** @brief The delivery the last DELIVERY record of the snapshot made. */
    uint32_t last_delivery;
} mqtt_restore;

/**
 * @brief An Application Message being routed, with its text not yet copied if
 * it can be helped.
 */
typedef struct {
    /** @brief Topic Name. */
    mqtt_utf8 topic_name;
    /** @brief The Properties to forward. */
    mqtt_payload properties;
    /**
     * @brief Payload, in the runs it arrived in, which stay valid while the
     * message is routed.
     */
    mqtt_run_list payload;
    /**
     * @brief The time the Message Expiry Interval ends, or zero if it has none.
     */
    uint64_t expiry_ms;
    /**
     * @brief The stored copy, or MQTT_SERVER_NONE if none has been needed so
     * far.
     */
    uint32_t message;
    /** @brief The Session that published it, or MQTT_SERVER_NONE. */
    uint32_t publisher;
    /** @brief QoS. */
    uint8_t qos;
    /** @brief RETAIN flag. */
    bool retain;
    /**
     * @brief true if properties may hold some that must not be forwarded as
     * they are.
     */
    bool properties_raw;
    /**
     * @brief Set if a Session that should have had the message could not be
     * given it.
     */
    bool lost;
} mqtt_route_source;

/** @brief What a connection takes blocks for, which sets how many it may. */
typedef enum {
    /**
     * @brief A packet the protocol sends in answer, which may use every free
     * block.
     */
    MQTT_SERVER_FOR_REPLY = 0,
    /** @brief Bytes received from the Client. */
    MQTT_SERVER_FOR_RECEIVE,
    /** @brief A PUBLISH packet going to the Client. */
    MQTT_SERVER_FOR_MESSAGE
} mqtt_server_purpose;

/**
 * @brief Give a message being routed a Payload that is in one place.
 * @param[out] source The message.
 * @param[out] run The run that holds the Payload, which must stay valid while
 * the message is routed.
 * @param[in] data The Payload.
 * @param[in] length Number of bytes in it.
 */
void mqtt_route_payload(mqtt_route_source *source, mqtt_run *run,
                        const uint8_t *data, uint32_t length);

/**
 * @brief Decide whether a message has too much before its Payload to ever be
 * sent.
 * @param[in] server The server.
 * @param[in] source The message.
 * @return true if it has.
 */
bool mqtt_route_too_large(const mqtt_server *server,
                          const mqtt_route_source *source);

/**
 * @brief Note that none of a Session's unfinished messages have been sent on
 * its new connection.
 * @param[in,out] server The server.
 * @param[in] session The Session.
 */
void mqtt_route_inflight_reset(mqtt_server *server, uint32_t session);

/**
 * @brief Drop the QoS 0 messages waiting for a Session, as those are never held
 * for a Client that has gone.
 * @param[in,out] server The server.
 * @param[in] session The Session.
 */
void mqtt_route_drop_queued_qos_0(mqtt_server *server, uint32_t session);

/**
 * @brief Store a copy of a message.
 * @param[in,out] server The server.
 * @param[in] source The message.
 * @return The message, with one reference held, or MQTT_SERVER_NONE if a pool
 * is exhausted.
 */
uint32_t mqtt_route_store(mqtt_server *server, const mqtt_route_source *source);

/**
 * @brief Add a reference to a stored message.
 * @param[in,out] server The server.
 * @param[in] message The message.
 */
void mqtt_route_hold(mqtt_server *server, uint32_t message);

/**
 * @brief Remove a reference from a stored message, and free it if that was the
 * last.
 * @param[in,out] server The server.
 * @param[in] message The message, or MQTT_SERVER_NONE to do nothing.
 */
void mqtt_route_release(mqtt_server *server, uint32_t message);

/**
 * @brief Send a message to everything that subscribes to its topic.
 * @param[in,out] server The server.
 * @param[in,out] source The message. A stored copy may be made, which the
 * caller releases.
 * @return The number of subscriptions it matched.
 */
uint32_t mqtt_route_publish(mqtt_server *server, mqtt_route_source *source);

/**
 * @brief Add a message to the end of a Session's queue.
 * @param[in,out] server The server.
 * @param[in] session The Session.
 * @param[in] message The stored message.
 * @param[in] qos The QoS to send it with.
 * @param[in] retain The RETAIN flag to send it with.
 * @param[in] identifier The Subscription Identifier, or zero.
 * @return true if it was queued, or false if there was no room and it was
 * dropped.
 */
bool mqtt_route_enqueue(mqtt_server *server, uint32_t session, uint32_t message,
                        uint8_t qos, bool retain, uint32_t identifier);

/**
 * @brief Decide whether a message's time is up.
 * @param[in] server The server.
 * @param[in] message The stored message.
 * @return true if it has a Message Expiry Interval that has passed.
 */
bool mqtt_route_expired(const mqtt_server *server, uint32_t message);

/**
 * @brief Put a connection on the list of those with work to do.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 */
void mqtt_route_mark_pending(mqtt_server *server, uint32_t connection);

/**
 * @brief Take a message off a Session's list of those sent and not
 * acknowledged, and free it.
 * @param[in,out] server The server.
 * @param[in] session The Session.
 * @param[in] delivery The delivery.
 */
void mqtt_route_inflight_remove(mqtt_server *server, uint32_t session,
                                uint32_t delivery);

/**
 * @brief Take a delivery from the pool.
 * @param[in,out] server The server.
 * @return The delivery, or MQTT_SERVER_NONE if the pool is exhausted.
 */
uint32_t mqtt_route_delivery_take(mqtt_server *server);

/**
 * @brief Return a delivery to the pool, releasing its message.
 * @param[in,out] server The server.
 * @param[in] delivery The delivery.
 */
void mqtt_route_delivery_give(mqtt_server *server, uint32_t delivery);

/**
 * @brief Move bytes waiting in a connection's Session into its ring, as far as
 * there is room.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 */
void mqtt_route_pump(mqtt_server *server, uint32_t connection);

/**
 * @brief Make a filter's text into a Session's subscription.
 * @param[in,out] server The server.
 * @param[in] session The Session.
 * @param[in] filter The Topic Filter, which has been checked.
 * @param[in] options The Subscription Options.
 * @param[in] identifier The Subscription Identifier, or zero.
 * @param[in] shared true for a Shared Subscription, in which case filter holds
 * the whole filter.
 * @param[out] was_new true if the Session did not have this subscription
 * already.
 * @return MQTT_OK or MQTT_QUOTA_EXCEEDED.
 */
mqtt_status mqtt_session_subscribe(mqtt_server *server, uint32_t session,
                                   const mqtt_utf8 *filter, uint8_t options,
                                   uint32_t identifier, bool shared,
                                   bool *was_new);

/**
 * @brief Remove a Session's subscription.
 * @param[in,out] server The server.
 * @param[in] session The Session.
 * @param[in] filter The Topic Filter.
 * @param[in] shared true for a Shared Subscription, in which case filter holds
 * the whole filter.
 * @return MQTT_OK or MQTT_NOT_FOUND.
 */
mqtt_status mqtt_session_unsubscribe(mqtt_server *server, uint32_t session,
                                     const mqtt_utf8 *filter, bool shared);

/**
 * @brief Queue the retained messages a new subscription should get.
 * @param[in,out] server The server.
 * @param[in] session The Session.
 * @param[in] filter The Topic Filter.
 * @param[in] qos The QoS granted.
 * @param[in] identifier The Subscription Identifier, or zero.
 */
void mqtt_session_send_retained(mqtt_server *server, uint32_t session,
                                const mqtt_utf8 *filter, uint8_t qos,
                                uint32_t identifier);

/**
 * @brief Store, replace or remove the retained message of a topic.
 * @param[in,out] server The server.
 * @param[in,out] source The message. A stored copy may be made, which the
 * caller releases.
 * @return MQTT_OK or MQTT_QUOTA_EXCEEDED.
 */
mqtt_status mqtt_session_retain(mqtt_server *server, mqtt_route_source *source);

/**
 * @brief Find the Session of a Client Identifier.
 * @param[in] server The server.
 * @param[in] identifier The Client Identifier.
 * @return The Session, or MQTT_SERVER_NONE.
 */
uint32_t mqtt_session_find(const mqtt_server *server,
                           const mqtt_utf8 *identifier);

/**
 * @brief Create a Session for a Client Identifier that has none.
 * @param[in,out] server The server.
 * @param[in] identifier The Client Identifier.
 * @return The Session, or MQTT_SERVER_NONE if a pool is exhausted.
 */
uint32_t mqtt_session_create(mqtt_server *server, const mqtt_utf8 *identifier);

/**
 * @brief Discard a Session and everything it holds, publishing any Will Message
 * it still has.
 * @param[in,out] server The server.
 * @param[in] session The Session.
 */
void mqtt_session_discard(mqtt_server *server, uint32_t session);

/**
 * @brief Publish a Session's Will Message and remove it.
 * @param[in,out] server The server.
 * @param[in] session The Session.
 */
void mqtt_session_publish_will(mqtt_server *server, uint32_t session);

/**
 * @brief Find the text of a Session's Client Identifier.
 * @param[in] server The server.
 * @param[in] session The Session.
 * @return The text.
 */
mqtt_utf8 mqtt_session_identifier(const mqtt_server *server, uint32_t session);

/**
 * @brief Put a whole packet in a connection's ring.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] bytes The packet.
 * @param[in] length Number of bytes.
 * @return MQTT_OK, or MQTT_BUFFER_TOO_SMALL if there is no room.
 */
mqtt_status mqtt_server_send(mqtt_server *server, uint32_t connection,
                             const uint8_t *bytes, uint32_t length);

/**
 * @brief Count the blocks a connection holds.
 * @param[in] entry The connection.
 * @return The number of blocks.
 */
uint32_t mqtt_server_held(const mqtt_connection *entry);

/**
 * @brief Count the blocks a connection may take now.
 * @param[in] server The server.
 * @param[in] entry The connection.
 * @param[in] purpose What they are for.
 * @return The number of blocks.
 */
uint32_t mqtt_server_grant(const mqtt_server *server,
                           const mqtt_connection *entry,
                           mqtt_server_purpose purpose);

/**
 * @brief Count the bytes that may go in a connection's transmit ring now.
 * @param[in] server The server.
 * @param[in] entry The connection.
 * @param[in] purpose What they are for: MQTT_SERVER_FOR_REPLY or
 * MQTT_SERVER_FOR_MESSAGE.
 * @return The number of bytes.
 */
uint32_t mqtt_server_room(const mqtt_server *server,
                          const mqtt_connection *entry,
                          mqtt_server_purpose purpose);

/**
 * @brief Put a connection at the back of those waiting for blocks, unless it
 * is there already.
 *
 * A connection waits only when nothing else will bring it back: it has no
 * bytes left to send, so no write will.
 *
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 */
void mqtt_server_wait_blocks(mqtt_server *server, uint32_t connection);

/**
 * @brief Note that blocks went back to the pool, and wake as many of the
 * connections waiting for them.
 *
 * A connection woken is marked as having work to do. One that still finds too
 * few blocks waits again at the back, so each wake-up is paid for by a block
 * coming back, and nothing goes round in a loop.
 *
 * @param[in,out] server The server.
 * @param[in] count Number of blocks.
 */
void mqtt_server_blocks_returned(mqtt_server *server, uint32_t count);

/**
 * @brief Check the pool and the quotas of a server's configuration and memory.
 * @param[in] config The configuration.
 * @param[in] storage The memory.
 * @return true if they can work together.
 */
bool mqtt_server_pool_valid(const mqtt_server_config *config,
                            const mqtt_server_storage *storage);

/**
 * @brief Handle a whole packet from a connection.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] header The Fixed Header.
 * @param[in] body The bytes after the Fixed Header.
 * @return MQTT_OK, or the reason the connection is closing.
 */
mqtt_status mqtt_server_handle_packet(mqtt_server *server, uint32_t connection,
                                      const mqtt_fixed_header *header,
                                      const uint8_t *body);

/**
 * @brief Close a connection because of a problem with what it sent.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] status The problem.
 */
void mqtt_server_fail(mqtt_server *server, uint32_t connection,
                      mqtt_status status);

/**
 * @brief Start closing a connection, and let the caller know it has work to do.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 */
void mqtt_server_begin_closing(mqtt_server *server, uint32_t connection);

/**
 * @brief Decide whether a server can be used: mqtt_server_init succeeded, and
 * its marker has not been overwritten since.
 * @param[in] server The server.
 * @return true if it can.
 */
bool mqtt_server_ready(const mqtt_server *server);

/**
 * @brief Decide whether an index names an open connection of a server that
 * can be used.
 * @param[in] server The server.
 * @param[in] connection The index.
 * @return true if the server is ready, the index is in the table, and its
 * entry is open.
 */
bool mqtt_server_connection_ready(const mqtt_server *server,
                                  uint32_t connection);

/**
 * @brief Start the time a Session has left, now that no connection has it.
 * @param[in,out] server The server.
 * @param[in] session The Session.
 */
void mqtt_server_start_expiry(mqtt_server *server, uint32_t session);

/**
 * @brief Do what the time says for a Session no connection has: its Will
 * Message, or its end.
 * @param[in,out] server The server.
 * @param[in] session The Session.
 */
void mqtt_server_session_timers(mqtt_server *server, uint32_t session);

/**
 * @brief Refuse a CONNECT packet, if the version allows it to be said.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] version The protocol the packet asked for.
 * @param[in] reason_code The Reason Code or the Connect Return Code.
 */
void mqtt_connect_refuse(mqtt_server *server, uint32_t connection,
                         mqtt_version version, uint8_t reason_code);

/**
 * @brief Handle a CONNECT packet.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] header The Fixed Header.
 * @param[in] body The bytes after the Fixed Header.
 * @return MQTT_OK, or the reason the connection is closing.
 */
mqtt_status mqtt_connect_handle(mqtt_server *server, uint32_t connection,
                                const mqtt_fixed_header *header,
                                const uint8_t *body);

/**
 * @brief Handle an AUTH packet.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] body The bytes after the Fixed Header.
 * @param[in] length Remaining Length.
 * @return MQTT_OK, or the reason to close the connection.
 */
mqtt_status mqtt_connect_handle_auth(mqtt_server *server, uint32_t connection,
                                     const uint8_t *body, uint32_t length);

/**
 * @brief Handle a PUBLISH packet whose Payload is in several runs.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] header The Fixed Header.
 * @param[in] head The bytes after the Fixed Header up to the Payload, in one
 * place.
 * @param[in] head_length Number of bytes in head.
 * @param[in] payload The Payload.
 * @return MQTT_OK, or the reason to close the connection.
 */
mqtt_status mqtt_publish_handle_runs(mqtt_server *server, uint32_t connection,
                                     const mqtt_fixed_header *header,
                                     const uint8_t *head, uint32_t head_length,
                                     const mqtt_run_list *payload);

/**
 * @brief Handle a PUBLISH packet.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] header The Fixed Header.
 * @param[in] body The bytes after the Fixed Header.
 * @return MQTT_OK, or the reason to close the connection.
 */
mqtt_status mqtt_publish_handle(mqtt_server *server, uint32_t connection,
                                const mqtt_fixed_header *header,
                                const uint8_t *body);

/**
 * @brief Handle a PUBACK, PUBREC, PUBREL or PUBCOMP packet.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] header The Fixed Header.
 * @param[in] body The bytes after the Fixed Header.
 * @return MQTT_OK, or the reason to close the connection.
 */
mqtt_status mqtt_publish_handle_response(mqtt_server *server,
                                         uint32_t connection,
                                         const mqtt_fixed_header *header,
                                         const uint8_t *body);

/**
 * @brief Add the fields of a SESSION record.
 * @param[in,out] builder The record.
 * @param[in] server The server.
 * @param[in] session The Session.
 */
void mqtt_journal_session_fields(mqtt_journal_builder *builder,
                                 const mqtt_server *server, uint32_t session);

/**
 * @brief Add the fields of a WILL record.
 * @param[in,out] builder The record.
 * @param[in] server The server.
 * @param[in] session The Session, which has a Will Message with a store id.
 */
void mqtt_journal_will_fields(mqtt_journal_builder *builder,
                              const mqtt_server *server, uint32_t session);

/**
 * @brief Add the fields of a MESSAGE record.
 * @param[in,out] builder The record.
 * @param[in] server The server.
 * @param[in] message The message, which has a store id.
 */
void mqtt_journal_message_fields(mqtt_journal_builder *builder,
                                 const mqtt_server *server, uint32_t message);

/**
 * @brief Add the fields of a DELIVERY record.
 * @param[in,out] builder The record.
 * @param[in] server The server.
 * @param[in] session The Session.
 * @param[in] delivery The delivery, which has a store id.
 */
void mqtt_journal_delivery_fields(mqtt_journal_builder *builder,
                                  const mqtt_server *server, uint32_t session,
                                  uint32_t delivery);

/**
 * @brief Add the fields of a SUBSCRIBE record, apart from the Topic Filter.
 * @param[in,out] builder The record.
 * @param[in] server The server.
 * @param[in] session The Session.
 * @param[in] options The Subscription Options.
 * @param[in] identifier The Subscription Identifier, or zero.
 * @param[in] shared true for a Shared Subscription.
 */
void mqtt_journal_subscribe_fields(mqtt_journal_builder *builder,
                                   const mqtt_server *server, uint32_t session,
                                   uint8_t options, uint32_t identifier,
                                   bool shared);

/**
 * @brief Let go every connection whose answers waited for groups now stored.
 * @param[in,out] server The server.
 */
void mqtt_journal_release(mqtt_server *server);

/**
 * @brief Give a store id to everything the snapshot about to begin has to
 * hold, and begin its walk.
 * @param[in,out] server The server, whose snapshot is beginning.
 */
void mqtt_snapshot_prepare(mqtt_server *server);

/**
 * @brief Start the Session the snapshot is writing again from its first
 * record, as it may have changed since that one.
 * @param[in,out] server The server.
 */
void mqtt_snapshot_changed(mqtt_server *server);

/**
 * @brief Describe a fault in the record being restored.
 * @param[in,out] restore The restore.
 * @param[in] kind What was wrong.
 * @return MQTT_STORE_DAMAGED.
 */
mqtt_status mqtt_restore_fail(mqtt_restore *restore,
                              mqtt_store_fault_kind kind);

/**
 * @brief Find what a store id names.
 * @param[in] restore The restore.
 * @param[in] id The store id.
 * @param[in] message true for a message, false for a delivery.
 * @return The index of the entry in map, or MQTT_SERVER_NONE.
 */
uint32_t mqtt_restore_find(const mqtt_restore *restore, uint64_t id,
                           bool message);

/**
 * @brief Add what a store id names.
 * @param[in,out] restore The restore.
 * @param[in] id The store id.
 * @param[in] index The message or delivery.
 * @param[in] session The Session of a delivery, or MQTT_SERVER_NONE.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED if map is full.
 */
mqtt_status mqtt_restore_add(mqtt_restore *restore, uint64_t id,
                             uint32_t index, uint32_t session);

/**
 * @brief Forget a store id: its entry stays, to keep the order, but names
 * nothing.
 * @param[in,out] restore The restore.
 * @param[in] entry The index of the entry in map.
 */
void mqtt_restore_forget(mqtt_restore *restore, uint32_t entry);

/**
 * @brief Put the entries of map in order of id.
 * @param[in,out] restore The restore.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED if two name the same thing.
 */
mqtt_status mqtt_restore_sort(mqtt_restore *restore);

/**
 * @brief Apply one record, other than MESSAGE, whose CRC has been checked.
 * @param[in,out] restore The restore, with the record's kind and offset.
 * @param[in] content The content.
 * @param[in] length Number of bytes of content.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
mqtt_status mqtt_restore_apply(mqtt_restore *restore, const uint8_t *content,
                               uint32_t length);

/**
 * @brief Check what the configuration says of the store.
 * @param[in] config The configuration.
 * @return true if both durabilities are ones the library knows.
 */
bool mqtt_journal_config_valid(const mqtt_server_config *config);

/**
 * @brief Set up the journal of a server being set up.
 * @param[in,out] server The server, whose configuration is set.
 */
void mqtt_journal_init(mqtt_server *server);

/**
 * @brief Write what the store keeps of a Session after its connection changed:
 * the Session and its Will Message if it lasts, or its end if it no longer
 * does.
 * @param[in,out] server The server.
 * @param[in] session The Session.
 */
void mqtt_journal_session(mqtt_server *server, uint32_t session);

/**
 * @brief Write that a Session has ended, if the store holds it.
 * @param[in,out] server The server.
 * @param[in] session The Session.
 */
void mqtt_journal_session_remove(mqtt_server *server, uint32_t session);

/**
 * @brief Write a subscription, new or replaced, of a Session the store holds.
 * @param[in,out] server The server.
 * @param[in] session The Session.
 * @param[in] filter The whole Topic Filter.
 * @param[in] options The Subscription Options.
 * @param[in] identifier The Subscription Identifier, or zero.
 * @param[in] shared true for a Shared Subscription.
 */
void mqtt_journal_subscribe(mqtt_server *server, uint32_t session,
                            const mqtt_utf8 *filter, uint8_t options,
                            uint32_t identifier, bool shared);

/**
 * @brief Write that a subscription of a Session the store holds has gone.
 * @param[in,out] server The server.
 * @param[in] session The Session.
 * @param[in] filter The whole Topic Filter.
 * @param[in] shared true for a Shared Subscription.
 */
void mqtt_journal_unsubscribe(mqtt_server *server, uint32_t session,
                              const mqtt_utf8 *filter, bool shared);

/**
 * @brief Write that a Session's Will Message is gone, if the store holds the
 * Session.
 * @param[in,out] server The server.
 * @param[in] session The Session.
 */
void mqtt_journal_will_remove(mqtt_server *server, uint32_t session);

/**
 * @brief Write that nothing holds a message any more, if the store holds it.
 * @param[in,out] server The server.
 * @param[in] message The message, whose last holder has let it go.
 */
void mqtt_journal_message_gone(mqtt_server *server, uint32_t message);

/**
 * @brief Write that a message's expiry has started, if the store holds it.
 * @param[in,out] server The server.
 * @param[in] message The message.
 */
void mqtt_journal_message_expiry(mqtt_server *server, uint32_t message);

/**
 * @brief Write that a message is the retained message of its Topic Name.
 * @param[in,out] server The server.
 * @param[in] message The message.
 */
void mqtt_journal_retained(mqtt_server *server, uint32_t message);

/**
 * @brief Write that a Topic Name has no retained message any more.
 * @param[in,out] server The server.
 * @param[in] topic_name The Topic Name.
 */
void mqtt_journal_retained_remove(mqtt_server *server,
                                  const mqtt_utf8 *topic_name);

/**
 * @brief Write a QoS 1 or QoS 2 delivery just queued for a Session the store
 * holds, and its message if the store doesn't hold that yet.
 * @param[in,out] server The server.
 * @param[in] session The Session.
 * @param[in] delivery The delivery.
 */
void mqtt_journal_delivery(mqtt_server *server, uint32_t session,
                           uint32_t delivery);

/**
 * @brief Write the Packet Identifier a QoS 2 delivery is being sent with.
 * @param[in,out] server The server.
 * @param[in] delivery The delivery, which has its Packet Identifier.
 */
void mqtt_journal_sent(mqtt_server *server, const mqtt_delivery *delivery);

/**
 * @brief Write that a QoS 2 delivery has had its PUBREC.
 * @param[in,out] server The server.
 * @param[in] delivery The delivery.
 */
void mqtt_journal_released(mqtt_server *server, uint32_t delivery);

/**
 * @brief Write that a delivery's flow is over, and forget its store id.
 * @param[in,out] server The server.
 * @param[in] delivery The delivery, about to be freed.
 */
void mqtt_journal_finished(mqtt_server *server, uint32_t delivery);

/**
 * @brief Write a QoS 2 Packet Identifier received from a Session's Client, or
 * its release.
 * @param[in,out] server The server.
 * @param[in] session The Session.
 * @param[in] packet_identifier The Packet Identifier.
 * @param[in] received true when it arrives, false when it is released.
 */
void mqtt_journal_incoming(mqtt_server *server, uint32_t session,
                           uint16_t packet_identifier, bool received);

/**
 * @brief Make an answer about to go in a connection's ring wait for the
 * records written so far to be stored. Nothing waits when everything written
 * is stored.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 */
void mqtt_journal_hold(mqtt_server *server, uint32_t connection);

/**
 * @brief Find whether a packet is an answer that waits for the journal.
 * @param[in] first_byte The first byte of its Fixed Header.
 * @return true for CONNACK, PUBACK, PUBREC, PUBREL, PUBCOMP, SUBACK and
 * UNSUBACK.
 */
bool mqtt_journal_holds(uint8_t first_byte);

#endif /* MQTT_BASE_SERVER_INTERNAL_H */
