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
 * @file base_client_internal.h
 * @brief What the source files of the client share. It is not part of the
 * library's interface.
 *
 * base_client.c holds what the application calls, the messages held for the
 * Server and the bytes waiting to go out. base_client_receive.c takes the bytes
 * that arrive and handles the packets the Server sends.
 * base_client_subscribe.c queues SUBSCRIBE and UNSUBSCRIBE packets.
 * base_client_store.c writes each change to the journal and holds back the
 * answers that wait for it, and base_client_restore.c reads a store back.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3.1.2.4, p.32; 3.2.2.1.1, p.45-46; 3.3.1.1, p.53; 4.1, p.92-93;
 * 4.4, p.96.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */
#ifndef MQTT_BASE_CLIENT_INTERNAL_H
#define MQTT_BASE_CLIENT_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "base_client.h"
#include "base_status.h"
#include "v5_properties.h"

/**
 * @brief Set up an event of one type, with nothing pointed to yet.
 * @param[out] event The event.
 * @param[in] type What happened.
 */
void mqtt_client_event_init(mqtt_client_event *event,
                            mqtt_client_event_type type);

/**
 * @brief Report an event, if anyone is interested.
 * @param[in] client The client.
 * @param[in] event The event.
 */
void mqtt_client_report(const mqtt_client *client,
                        const mqtt_client_event *event);

/**
 * @brief Start closing the connection.
 * @param[in,out] client The client.
 */
void mqtt_client_begin_closing(mqtt_client *client);

/**
 * @brief Decide whether a client can be used: mqtt_client_init succeeded, and
 * its marker has not been overwritten since.
 * @param[in] client The client.
 * @return true if it can.
 */
bool mqtt_client_ready(const mqtt_client *client);

/**
 * @brief Pick an unused Packet Identifier: [MQTT-2.2.1-3].
 * @param[in,out] client The client.
 * @return The Packet Identifier, or zero if every value is in use.
 */
uint16_t mqtt_client_next_identifier(mqtt_client *client);

/**
 * @brief Put a whole packet in the ring.
 * @param[in,out] client The client.
 * @param[in] bytes The packet.
 * @param[in] length Number of bytes.
 * @return MQTT_OK, or MQTT_BUFFER_TOO_SMALL if there is no room.
 */
mqtt_status mqtt_client_send(mqtt_client *client, const uint8_t *bytes,
                             uint32_t length);

/**
 * @brief Count the bytes that fit in the ring, with every free block.
 * @param[in] client The client.
 * @return The number of bytes.
 */
uint32_t mqtt_client_ring_free(const mqtt_client *client);

/**
 * @brief Close because of a problem with what the Server sent, saying why if
 * that can be said.
 * @param[in,out] client The client.
 * @param[in] status The problem.
 */
void mqtt_client_fail(mqtt_client *client, mqtt_status status);

/**
 * @brief Send a PUBREL packet that a reconnection has made due again.
 * @param[in,out] client The client.
 * @param[in] packet_identifier The Packet Identifier.
 * @return The outcome.
 */
mqtt_status mqtt_client_resend_release(mqtt_client *client,
                                       uint16_t packet_identifier);

/**
 * @brief Take a message from the pool and put it at the end of the list. The
 * caller has checked that one is free.
 * @param[in,out] client The client.
 * @param[in] block The block with the bytes.
 * @param[in] size Number of bytes.
 * @param[in] packet_identifier The Packet Identifier, or zero.
 * @return The message.
 */
uint32_t mqtt_client_message_add(mqtt_client *client, uint32_t block,
                                 uint32_t size, uint16_t packet_identifier);

/**
 * @brief Take a message off the list and free it.
 * @param[in,out] client The client.
 * @param[in] index The message.
 */
void mqtt_client_message_remove(mqtt_client *client, uint32_t index);

/**
 * @brief Find a message that has been sent, by its Packet Identifier.
 * @param[in] client The client.
 * @param[in] packet_identifier The Packet Identifier.
 * @return The message, or MQTT_CLIENT_NONE.
 */
uint32_t mqtt_client_message_find(const mqtt_client *client,
                                  uint16_t packet_identifier);

/**
 * @brief Discard all that is held about the Session: [MQTT-3.1.2-4],
 * [MQTT-3.2.2-5].
 *
 * A message that has never been sent is not part of the Session State, which
 * section 4.1 defines, so it stays to go out on the new Session.
 *
 * @param[in,out] client The client.
 */
void mqtt_client_discard_session(mqtt_client *client);

/**
 * @brief Mark the messages sent on an earlier connection as resends.
 *
 * Rules [MQTT-3.3.1-1] and [MQTT-4.4.0-1] apply here.
 *
 * @param[in,out] client The client.
 */
void mqtt_client_mark_resends(mqtt_client *client);

/**
 * @brief Give up the held messages this Server's limits rule out, and say so to
 * the application.
 *
 * It looks from the start after each report, as the application's callback may
 * change the list. The connection is not active yet, so nothing can go out
 * while this runs.
 *
 * @param[in,out] client The client.
 */
void mqtt_client_refuse_held(mqtt_client *client);

/**
 * @brief Move held messages into the ring, in order, as room and the send quota
 * allow.
 *
 * A message past PUBREC only needs its PUBREL.
 *
 * @param[in,out] client The client.
 */
void mqtt_client_pump(mqtt_client *client);

/**
 * @brief Decide whether Properties name the Authentication Method this Client
 * began with.
 * @param[in] client The client.
 * @param[in] properties The Properties of a packet.
 * @return true if they do, or if the method couldn't be kept to compare with.
 */
bool mqtt_client_method_matches(const mqtt_client *client,
                                const mqtt5_properties *properties);

/**
 * @brief Set up the journal of a client being set up: it keeps nothing.
 * @param[out] client The client.
 */
void mqtt_client_journal_init(mqtt_client *client);

/**
 * @brief Keep the Client Identifier of the Session, as a CONNECT packet gives
 * it or the Server assigns it.
 * @param[in,out] client The client.
 * @param[in] identifier The identifier. An empty one is kept as unknown.
 */
void mqtt_client_journal_identifier(mqtt_client *client,
                                    const mqtt_utf8 *identifier);

/**
 * @brief Write a CLIENT_SESSION record, if the version, whether a Session
 * exists, or its Client Identifier has changed since the last.
 * @param[in,out] client The client.
 */
void mqtt_client_journal_session(mqtt_client *client);

/**
 * @brief Write a CLIENT_MESSAGE record for a QoS 1 or QoS 2 message just held.
 * @param[in,out] client The client.
 * @param[in] index The message.
 */
void mqtt_client_journal_message(mqtt_client *client, uint32_t index);

/**
 * @brief Write a record that names a Packet Identifier alone: CLIENT_SENT,
 * CLIENT_RELEASED, CLIENT_FINISHED, CLIENT_INCOMING, CLIENT_INCOMING_REMOVE or
 * CLIENT_PENDING_DONE.
 * @param[in,out] client The client.
 * @param[in] kind The kind of record.
 * @param[in] packet_identifier The Packet Identifier.
 */
void mqtt_client_journal_mark(mqtt_client *client, uint8_t kind,
                              uint16_t packet_identifier);

/**
 * @brief Write a CLIENT_PENDING record, with MQTT 3.1, for a SUBSCRIBE or
 * UNSUBSCRIBE packet about to go.
 * @param[in,out] client The client.
 * @param[in] packet_identifier The Packet Identifier.
 * @param[in] bytes The packet.
 * @param[in] length Number of bytes in it.
 */
void mqtt_client_journal_pending(mqtt_client *client,
                                 uint16_t packet_identifier,
                                 const uint8_t *bytes, uint32_t length);

/**
 * @brief Note that a held message's PUBLISH packet is about to go: write
 * CLIENT_SENT the first time, and make a QoS 2 one wait for the store.
 * @param[in,out] client The client.
 * @param[in] entry The message.
 * @param[in] direct true if it goes straight from the arena.
 */
void mqtt_client_journal_sending(mqtt_client *client,
                                 const mqtt_client_message *entry,
                                 bool direct);

/**
 * @brief Make a packet about to be added to the ring wait for the store, if
 * it is one that tells the Server something was kept and records are not
 * stored yet.
 * @param[in,out] client The client.
 * @param[in] bytes The packet.
 * @param[in] length Number of bytes in it.
 */
void mqtt_client_journal_hold(mqtt_client *client, const uint8_t *bytes,
                              uint32_t length);

/**
 * @brief Find the next bytes to send, as far as the store allows.
 * @param[in] client The client.
 * @param[out] runs The runs.
 * @param[in] max_runs Number of entries runs can hold.
 * @return Number of runs.
 */
uint32_t mqtt_client_journal_view(const mqtt_client *client, mqtt_run *runs,
                                  uint32_t max_runs);

/**
 * @brief Take bytes sent from the ring off those that may go before the
 * answers that wait.
 * @param[in,out] client The client.
 * @param[in] count Number of bytes.
 */
void mqtt_client_journal_consumed(mqtt_client *client, uint32_t count);

/**
 * @brief Forget the answers that waited, as the bytes of the connection have
 * gone.
 * @param[in,out] client The client.
 */
void mqtt_client_journal_lost(mqtt_client *client);

#endif /* MQTT_BASE_CLIENT_INTERNAL_H */
