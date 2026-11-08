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
 * @file base_client_subscribe.c
 * @brief The MQTT Client's SUBSCRIBE and UNSUBSCRIBE packets: checking the
 * Topic Filters, taking a Packet Identifier, and queueing the packet until its
 * answer comes.
 *
 * base_client_receive.c handles the SUBACK and UNSUBACK packets.
 *
 * [V31] MQTT V3.1 Protocol Specification.
 * @li Cited: 3.8, 3.10.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015.
 * @li Cited: 3.8, 3.10.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3.8, 3.10, p.73-86.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */

#include <stddef.h>

#include "base_client_internal.h"
#include "base_qos.h"
#include "base_string.h"
#include "base_topic_check.h"
#include "base_type.h"
#include "base_utf8_check.h"
#include "v5_prop.h"
#include "v5_rc.h"
#include "v5_subscribe.h"

/**
 * @brief Take a place for a packet that waits for an answer, and pick its
 * Packet Identifier.
 * @param[in,out] client The client.
 * @param[out] packet_identifier The Packet Identifier.
 * @return The index of the place, or MQTT_CLIENT_NONE if all are in use.
 */
static uint32_t client_pending_take(mqtt_client *client,
                                    uint16_t *packet_identifier)
{
    /** @brief Index of the entry being looked at. */
    uint32_t index = 0U;
    /** @brief The place found. */
    uint32_t found = MQTT_CLIENT_NONE;

    while ((found == MQTT_CLIENT_NONE) &&
           (index < client->storage.pending_count)) {
        if (client->storage.pending[index].packet_identifier == 0U) {
            found = index;
        }
        index += 1U;
    }
    if (found != MQTT_CLIENT_NONE) {
        *packet_identifier = mqtt_client_next_identifier(client);
        if (*packet_identifier == 0U) {
            found = MQTT_CLIENT_NONE;
        }
    }
    return found;
}

/**
 * @brief Encode a SUBSCRIBE or UNSUBSCRIBE Payload.
 * @param[in,out] writer The writer.
 * @param[in] entries The entries of a SUBSCRIBE, or NULL.
 * @param[in] filters The Topic Filters of an UNSUBSCRIBE, or NULL.
 * @param[in] count Number of entries.
 */
static void client_write_filters(mqtt_writer *writer,
                                 const mqtt_client_subscription *entries,
                                 const mqtt_utf8 *filters, uint32_t count)
{
    /** @brief Index of the entry. */
    uint32_t index = 0U;

    while (index < count) {
        if (entries != NULL) {
            mqtt_write_utf8(writer, &entries[index].topic_filter);
            mqtt_write_byte(writer, entries[index].options);
        } else {
            mqtt_write_utf8(writer, &filters[index]);
        }
        index += 1U;
    }
}

/**
 * @brief Refuse a Topic Filter, or Subscription Options, that a Server would
 * only close on.
 * @param[in] client The client.
 * @param[in] entries The entries of a SUBSCRIBE, or NULL.
 * @param[in] filters The filters of an UNSUBSCRIBE, or NULL.
 * @param[in] count Number of entries.
 * @return MQTT_OK, MQTT_TOPIC_FILTER_INVALID, or MQTT_INVALID_ARGUMENT.
 */
static mqtt_status client_check_filters(const mqtt_client *client,
                                        const mqtt_client_subscription *entries,
                                        const mqtt_utf8 *filters,
                                        uint32_t count)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief Index of the entry. */
    uint32_t index = 0U;
    /**
     * @brief The bits of the Subscription Options that this version has no use
     * for.
     */
    const uint32_t unused = (client->version == MQTT_VERSION_5) ?
                                MQTT5_SUBSCRIPTION_OPTIONS_RESERVED :
                                (0xFFU & ~MQTT5_MAXIMUM_QOS_MASK);

    while ((status == MQTT_OK) && (index < count)) {
        /** @brief The filter. */
        const mqtt_utf8 *filter =
            (entries != NULL) ? &entries[index].topic_filter : &filters[index];

        status = mqtt_topic_filter_check(filter, client->version);
        if ((status == MQTT_OK) && (entries != NULL)) {
            /** @brief The Subscription Options. */
            const uint32_t options = (uint32_t)entries[index].options;

            if (((options & unused) != 0U) ||
                ((options & MQTT5_MAXIMUM_QOS_MASK) == MQTT_QOS_RESERVED) ||
                (((options & MQTT5_RETAIN_HANDLING_MASK) >>
                  MQTT5_RETAIN_HANDLING_POS) ==
                 MQTT5_RETAIN_HANDLING_RESERVED)) {
                status = MQTT_INVALID_ARGUMENT;
            }
        }
        index += 1U;
    }
    return status;
}

/**
 * @brief Refuse a SUBSCRIBE or UNSUBSCRIBE packet that cannot go out now, or
 * that a Server would only close on.
 * @param[in] client The client.
 * @param[in] entries The entries of a SUBSCRIBE, or NULL.
 * @param[in] filters The Topic Filters of an UNSUBSCRIBE, or NULL.
 * @param[in] count Number of entries.
 * @return MQTT_OK, or why it was not accepted.
 */
static mqtt_status client_check_request(const mqtt_client *client,
                                        const mqtt_client_subscription *entries,
                                        const mqtt_utf8 *filters,
                                        uint32_t count)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (client->state != MQTT_CLIENT_ACTIVE) {
        status = MQTT_INVALID_STATE;
    } else if (count == 0U) {
        status = MQTT_INVALID_ARGUMENT;
    } else {
        status = client_check_filters(client, entries, filters, count);
    }
    return status;
}

/**
 * @brief Fill in a place for a packet that waits for an answer, keeping a copy
 * of the packet with MQTT 3.1, which sends it again.
 * @param[in,out] client The client.
 * @param[in] place The place.
 * @param[in] packet_identifier The Packet Identifier.
 * @param[in] bytes The packet.
 * @param[in] length Number of bytes in the packet.
 */
static void client_pending_fill(mqtt_client *client, uint32_t place,
                                uint16_t packet_identifier,
                                const uint8_t *bytes, uint32_t length)
{
    /** @brief The entry. */
    mqtt_client_pending *entry = &client->storage.pending[place];

    entry->packet_identifier = packet_identifier;
    /* If there is no room for this, the worst is that it isn't sent again. */
    if (client->version == MQTT_VERSION_31) {
        entry->block = mqtt_arena_take(&client->arena, length);
        if (entry->block != MQTT_ARENA_NONE) {
            /* The block was just taken for the packet, so it fits. */
            mqtt_bytes_put(mqtt_arena_bytes(&client->arena, entry->block),
                           bytes, length);
            entry->length = length;
        }
    }
}

/**
 * @brief Put a SUBSCRIBE or UNSUBSCRIBE packet in the ring. With MQTT 3.1 it
 * is kept in the journal before it goes, and its record taken back if it
 * can't.
 * @param[in,out] client The client.
 * @param[in] packet_identifier The Packet Identifier.
 * @param[in] bytes The packet.
 * @param[in] length Number of bytes in the packet.
 * @return MQTT_OK, or MQTT_BUFFER_TOO_SMALL if there is no room.
 */
static mqtt_status client_send_request(mqtt_client *client,
                                       uint16_t packet_identifier,
                                       const uint8_t *bytes, uint32_t length)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    mqtt_client_journal_pending(client, packet_identifier, bytes, length);
    status = mqtt_client_send(client, bytes, length);
    if (status != MQTT_OK) {
        mqtt_client_journal_mark(client,
                                 (uint8_t)MQTT_STORE_RECORD_CLIENT_PENDING_DONE,
                                 packet_identifier);
    }
    return status;
}

/**
 * @brief Queue a SUBSCRIBE or UNSUBSCRIBE packet.
 * @param[in,out] client The client.
 * @param[in] entries The entries of a SUBSCRIBE, or NULL.
 * @param[in] filters The Topic Filters of an UNSUBSCRIBE, or NULL.
 * @param[in] count Number of entries.
 * @param[in] properties The Properties, or NULL for none.
 * @param[out] packet_identifier The Packet Identifier.
 * @return MQTT_OK, or why it was not accepted.
 */
static mqtt_status client_send_filters(mqtt_client *client,
                                       const mqtt_client_subscription *entries,
                                       const mqtt_utf8 *filters, uint32_t count,
                                       const mqtt5_properties *properties,
                                       uint16_t *packet_identifier)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief A writer that only counts. */
    mqtt_writer counter = {NULL, 0U, 0U, MQTT_OK};

    mqtt_writer_init(&counter, NULL, 0U);
    client_write_filters(&counter, entries, filters, count);
    status = client_check_request(client, entries, filters, count);
    if (status == MQTT_OK) {
        /** @brief A block for the Payload and then the packet. */
        const uint32_t block =
            mqtt_arena_take(&client->arena, (counter.position * 2U) + 4096U);

        if (block == MQTT_ARENA_NONE) {
            status = MQTT_QUOTA_EXCEEDED;
        } else {
            /** @brief The bytes of the block. */
            uint8_t *bytes = mqtt_arena_bytes(&client->arena, block);
            /** @brief A writer. */
            mqtt_writer writer = {NULL, 0U, 0U, MQTT_OK};
            /** @brief The packet. */
            mqtt_subscribe packet;
            /** @brief The place for the Packet Identifier. */
            const uint32_t place =
                client_pending_take(client, packet_identifier);

            mqtt_subscribe_clear(&packet);
            if (place == MQTT_CLIENT_NONE) {
                status = MQTT_QUOTA_EXCEEDED;
            } else {
                mqtt_writer_init(&writer, bytes, counter.position);
                client_write_filters(&writer, entries, filters, count);
                packet.packet_identifier = *packet_identifier;
                packet.payload.data = bytes;
                packet.payload.length = writer.position;
                if (properties != NULL) {
                    /* Copied as bytes, as assigning a struct this size would
                     * be done with a call to memcpy. */
                    mqtt_bytes_put((uint8_t *)&packet.properties,
                                   (const uint8_t *)properties,
                                   (uint32_t)sizeof(*properties));
                }
                mqtt_writer_init(&writer, &bytes[counter.position],
                                 counter.position + 4096U);
                if (entries != NULL) {
                    mqtt_subscribe_encode(&writer, client->version, &packet);
                } else {
                    mqtt_unsubscribe_encode(&writer, client->version, &packet);
                }
                status = writer.status;
                if ((status == MQTT_OK) &&
                    (writer.position > client->server_maximum_packet_size)) {
                    /* [MQTT-3.2.2-15] */
                    status = MQTT_PACKET_TOO_LARGE;
                }
                if (status == MQTT_OK) {
                    status = client_send_request(client, *packet_identifier,
                                                 &bytes[counter.position],
                                                 writer.position);
                }
                if (status == MQTT_OK) {
                    client_pending_fill(client, place, *packet_identifier,
                                        &bytes[counter.position],
                                        writer.position);
                }
            }
            mqtt_arena_give(&client->arena, block);
        }
    }
    return status;
}

/**
 * @brief Do what mqtt_client_subscribe does, for a client known to be ready.
 * @param[in,out] client The client.
 * @param[in] entries The Topic Filters.
 * @param[in] count Number of entries.
 * @param[in] properties The Properties, or NULL for none.
 * @param[out] packet_identifier The Packet Identifier the answer will have.
 * @return MQTT_OK, or why it was not accepted.
 */
static mqtt_status client_subscribe_body(
    mqtt_client *client, const mqtt_client_subscription *entries,
    uint32_t count, const mqtt5_properties *properties,
    uint16_t *packet_identifier)
{
    return client_send_filters(client, entries, NULL, count, properties,
                               packet_identifier);
}

mqtt_status mqtt_client_subscribe(mqtt_client *client,
                                  const mqtt_client_subscription *entries,
                                  uint32_t count,
                                  const mqtt5_properties *properties,
                                  uint16_t *packet_identifier)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if (mqtt_client_ready(client) && ((entries != NULL) || (count == 0U)) &&
        (packet_identifier != NULL)) {
        answer = client_subscribe_body(client, entries, count, properties,
                                       packet_identifier);
    }
    return answer;
}

/**
 * @brief Do what mqtt_client_unsubscribe does, for a client known to be ready.
 * @param[in,out] client The client.
 * @param[in] filters The Topic Filters.
 * @param[in] count Number of entries.
 * @param[in] properties The Properties, or NULL for none.
 * @param[out] packet_identifier The Packet Identifier the answer will have.
 * @return MQTT_OK, or why it was not accepted.
 */
static mqtt_status client_unsubscribe_body(mqtt_client *client,
                                           const mqtt_utf8 *filters,
                                           uint32_t count,
                                           const mqtt5_properties *properties,
                                           uint16_t *packet_identifier)
{
    return client_send_filters(client, NULL, filters, count, properties,
                               packet_identifier);
}

mqtt_status mqtt_client_unsubscribe(mqtt_client *client,
                                    const mqtt_utf8 *filters, uint32_t count,
                                    const mqtt5_properties *properties,
                                    uint16_t *packet_identifier)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if (mqtt_client_ready(client) && ((filters != NULL) || (count == 0U)) &&
        (packet_identifier != NULL)) {
        answer = client_unsubscribe_body(client, filters, count, properties,
                                         packet_identifier);
    }
    return answer;
}
