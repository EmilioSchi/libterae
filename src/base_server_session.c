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
 * @file base_server_session.c
 * @brief Sessions, subscriptions and retained messages.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3.1.2.4, 3.1.3.2, 3.8.4, 3.10.4, 4.1, 4.8.2, p.44-110.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015.
 * @li Cited: 3.1.2.4, 3.8.4, 3.10.4, 4.1.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * A Session is found by its Client Identifier in a table that keeps the
 * Sessions in order of it, by halving the part of the table that could hold
 * it. Nothing is hashed: a Client chooses its Client Identifier, and no choice
 * makes a search take more than one comparison for each halving.
 */

#include <stddef.h>

#include "base_qos.h"
#include "base_server_internal.h"
#include "base_string.h"
#include "base_topic_check.h"

mqtt_utf8 mqtt_session_identifier(const mqtt_server *server, uint32_t session)
{
    /** @brief The Session. */
    const mqtt_session *entry = &server->storage.sessions[session];
    /** @brief The view. */
    mqtt_utf8 identifier = {0U, NULL};

    identifier.length = entry->identifier_length;
    identifier.data =
        mqtt_page_arena_bytes(&server->arena, entry->identifier_block);
    return identifier;
}

/**
 * @brief Put a Client Identifier in order with that of a Session: the shorter
 * comes first, and of two as long, the one whose first byte that differs is
 * smaller.
 * @param[in] server The server.
 * @param[in] identifier The Client Identifier.
 * @param[in] session The Session.
 * @return Below zero if the Client Identifier comes first, zero if they are
 * the same, and above zero if it comes after.
 */
static int32_t session_order(const mqtt_server *server,
                             const mqtt_utf8 *identifier, uint32_t session)
{
    /** @brief The Session's Client Identifier. */
    const mqtt_utf8 other = mqtt_session_identifier(server, session);
    /** @brief Index of the byte being compared. */
    uint32_t index = 0U;
    /** @brief The answer. */
    int32_t order = 0;

    if (identifier->length != other.length) {
        order = (identifier->length < other.length) ? -1 : 1;
    }
    while ((order == 0) && (index < (uint32_t)identifier->length)) {
        if (identifier->data[index] != other.data[index]) {
            order = (identifier->data[index] < other.data[index]) ? -1 : 1;
        }
        index += 1U;
    }
    return order;
}

/**
 * @brief Find where a Client Identifier is, or would go, in the table.
 * @param[in] server The server.
 * @param[in] identifier The Client Identifier.
 * @param[out] position Index of the first entry that doesn't come before it.
 * @return The Session at that index if it has this Client Identifier, or
 * MQTT_SERVER_NONE.
 */
static uint32_t session_search(const mqtt_server *server,
                               const mqtt_utf8 *identifier, uint32_t *position)
{
    /** @brief Every entry before this one comes before the identifier. */
    uint32_t low = 0U;
    /** @brief No entry from this one on comes before it. */
    uint32_t high = server->session_table_used;
    /** @brief The Session found. */
    uint32_t found = MQTT_SERVER_NONE;

    while (low < high) {
        /** @brief The entry halfway between. */
        const uint32_t middle = low + ((high - low) / 2U);
        /** @brief Its Session. */
        const uint32_t session = server->storage.session_table[middle];
        /** @brief How the Client Identifier compares with it. */
        const int32_t order = session_order(server, identifier, session);

        if (order > 0) {
            low = middle + 1U;
        } else {
            high = middle;
            if (order == 0) {
                found = session;
            }
        }
    }
    *position = low;
    return found;
}

uint32_t mqtt_session_find(const mqtt_server *server,
                           const mqtt_utf8 *identifier)
{
    /** @brief Where it is, or would go. */
    uint32_t position = 0U;

    return session_search(server, identifier, &position);
}

/**
 * @brief Put a Session in the table at its place in the order, and move the
 * entries after it up by one.
 *
 * The Session has just been taken from session_slots, so the table holds
 * fewer than session_count entries, and has room for one more.
 *
 * @param[in,out] server The server.
 * @param[in] session The Session, whose Client Identifier is set and in no
 * other Session.
 */
static void session_table_insert(mqtt_server *server, uint32_t session)
{
    /** @brief Its Client Identifier. */
    const mqtt_utf8 identifier = mqtt_session_identifier(server, session);
    /** @brief The table. */
    uint32_t *table = server->storage.session_table;
    /** @brief Where it goes. */
    uint32_t position = 0U;
    /** @brief Index of the entry being moved up into. */
    uint32_t index = server->session_table_used;

    (void)session_search(server, &identifier, &position);
    while (index > position) {
        table[index] = table[index - 1U];
        index -= 1U;
    }
    table[position] = session;
    server->session_table_used += 1U;
}

uint32_t mqtt_session_create(mqtt_server *server, const mqtt_utf8 *identifier)
{
    /** @brief The Session. */
    uint32_t index = mqtt_slab_take(&server->session_slots);

    if (index != MQTT_SLAB_NONE) {
        /** @brief The Session. */
        mqtt_session *entry = &server->storage.sessions[index];
        /** @brief The block for the Client Identifier. */
        const uint32_t block =
            mqtt_page_arena_take(&server->arena, (uint32_t)identifier->length);

        if (block == MQTT_PAGE_ARENA_NONE) {
            (void)mqtt_slab_give(&server->session_slots, index);
            index = MQTT_SERVER_NONE;
        } else {
            entry->connection = MQTT_SERVER_NONE;
            entry->identifier_block = block;
            entry->identifier_length = identifier->length;
            entry->subscriptions = MQTT_SERVER_NONE;
            entry->queued_head = MQTT_SERVER_NONE;
            entry->queued_tail = MQTT_SERVER_NONE;
            entry->queued_count = 0U;
            entry->inflight_head = MQTT_SERVER_NONE;
            entry->inflight_tail = MQTT_SERVER_NONE;
            entry->incoming_head = MQTT_SERVER_NONE;
            entry->incoming_count = 0U;
            entry->will_message = MQTT_SERVER_NONE;
            entry->will_delay_s = 0U;
            entry->will_expiry_s = 0U;
            entry->will_has_expiry = false;
            entry->expiry_s = 0U;
            entry->will_due_ms = 0U;
            entry->expiry_due_ms = 0U;
            entry->last_packet_identifier = 0U;
            entry->identifier_boundary = MQTT_SERVER_NONE;
            entry->in_use = true;
            /* The block was taken for the identifier, so it fits. */
            if (identifier->length > 0U) {
                mqtt_bytes_put(mqtt_page_arena_bytes(&server->arena, block),
                               identifier->data, (uint32_t)identifier->length);
            }
            session_table_insert(server, index);
        }
    }
    return index;
}

/**
 * @brief Take a Session out of the table, and move the entries after it down
 * by one.
 * @param[in,out] server The server.
 * @param[in] session The Session, which is in the table.
 */
static void session_table_remove(mqtt_server *server, uint32_t session)
{
    /** @brief Its Client Identifier. */
    const mqtt_utf8 identifier = mqtt_session_identifier(server, session);
    /** @brief The table. */
    uint32_t *table = server->storage.session_table;
    /** @brief Index of the entry being moved down into. */
    uint32_t index = 0U;

    /* No two Sessions have the same Client Identifier, so the first entry
     * that doesn't come before it is its own. */
    (void)session_search(server, &identifier, &index);
    if ((index < server->session_table_used) && (table[index] == session)) {
        server->session_table_used -= 1U;
        while (index < server->session_table_used) {
            table[index] = table[index + 1U];
            index += 1U;
        }
    }
}

/**
 * @brief Take a subscription out of its node or group, and free the group if it
 * empties.
 * @param[in,out] server The server.
 * @param[in] subscription The subscription.
 */
static void session_unlink_here(mqtt_server *server, uint32_t subscription)
{
    /** @brief The subscription. */
    const mqtt_subscription *item =
        &server->storage.subscriptions[subscription];
    /** @brief The node. */
    mqtt_tree_node *node = &server->tree.nodes[item->node];

    if (item->next_here != MQTT_SERVER_NONE) {
        server->storage.subscriptions[item->next_here].previous_here =
            item->previous_here;
    }
    if (item->previous_here != MQTT_SERVER_NONE) {
        server->storage.subscriptions[item->previous_here].next_here =
            item->next_here;
    } else if (item->group == MQTT_SERVER_NONE) {
        node->subscriptions = item->next_here;
    } else {
        server->storage.groups[item->group].members = item->next_here;
    }
    if (item->group != MQTT_SERVER_NONE) {
        /** @brief The group. */
        mqtt_share_group *group = &server->storage.groups[item->group];

        if (group->cursor == subscription) {
            group->cursor = item->next_here;
        }
        if (group->members == MQTT_SERVER_NONE) {
            if (group->previous == MQTT_SERVER_NONE) {
                node->groups = group->next;
            } else {
                server->storage.groups[group->previous].next = group->next;
            }
            if (group->next != MQTT_SERVER_NONE) {
                server->storage.groups[group->next].previous = group->previous;
            }
            mqtt_page_arena_give(&server->arena, group->name_block);
            (void)mqtt_slab_give(&server->group_slots, item->group);
        }
    }
}

/**
 * @brief Remove a subscription from everything that refers to it, and free it.
 * @param[in,out] server The server.
 * @param[in] subscription The subscription.
 */
static void session_subscription_free(mqtt_server *server,
                                      uint32_t subscription)
{
    /** @brief The subscription. */
    mqtt_subscription *item = &server->storage.subscriptions[subscription];
    /** @brief The Session. */
    mqtt_session *session = &server->storage.sessions[item->session];
    /** @brief The node. */
    const uint32_t node = item->node;

    session_unlink_here(server, subscription);
    if (item->previous_of_session == MQTT_SERVER_NONE) {
        session->subscriptions = item->next_of_session;
    } else {
        /** @brief The subscription before it in the Session. */
        mqtt_subscription *earlier =
            &server->storage.subscriptions[item->previous_of_session];

        earlier->next_of_session = item->next_of_session;
    }
    if (item->next_of_session != MQTT_SERVER_NONE) {
        /** @brief The subscription after it in the Session. */
        mqtt_subscription *later =
            &server->storage.subscriptions[item->next_of_session];

        later->previous_of_session = item->previous_of_session;
    }
    (void)mqtt_slab_give(&server->subscription_slots, subscription);
    mqtt_tree_prune(&server->tree, node);
}

/**
 * @brief Free every entry of a list of deliveries.
 * @param[in,out] server The server.
 * @param[in] head The first entry.
 */
static void session_free_deliveries(mqtt_server *server, uint32_t head)
{
    /** @brief The entry being freed. */
    uint32_t index = head;

    while (index != MQTT_SERVER_NONE) {
        /** @brief The entry after it. */
        const uint32_t next = server->storage.deliveries[index].next;

        /* The end of the Session covers its deliveries in the store. */
        server->storage.deliveries[index].store_id = 0U;
        mqtt_route_delivery_give(server, index);
        index = next;
    }
}

void mqtt_session_publish_will(mqtt_server *server, uint32_t session)
{
    /** @brief The Session. */
    mqtt_session *entry = &server->storage.sessions[session];

    if (entry->will_message != MQTT_SERVER_NONE) {
        /** @brief The stored Will Message. */
        const mqtt_message *message =
            &server->storage.messages[entry->will_message];
        /** @brief The bytes of the Will Message. */
        const uint8_t *bytes =
            mqtt_page_arena_bytes(&server->arena, message->block);
        /** @brief The Will Message, as a message being routed. */
        mqtt_route_source source;
        /** @brief The run that holds its Payload. */
        mqtt_run run = {NULL, 0U};

        source.topic_name.data = bytes;
        source.topic_name.length = message->topic_length;
        source.properties.data = &bytes[message->topic_length];
        source.properties.length = message->properties_length;
        source.lost = false;
        mqtt_route_payload(&source, &run,
                           &bytes[(uint32_t)message->topic_length +
                                  message->properties_length],
                           message->payload_length);
        source.expiry_ms = 0U;
        if (entry->will_has_expiry) {
            source.expiry_ms =
                server->now_ms + ((uint64_t)entry->will_expiry_s * 1000U);
            server->storage.messages[entry->will_message].expiry_ms =
                source.expiry_ms;
            mqtt_journal_message_expiry(server, entry->will_message);
        }
        mqtt_journal_will_remove(server, session);
        source.message = entry->will_message;
        source.publisher = session;
        source.qos = message->qos;
        source.retain = message->retain;
        source.properties_raw = false;
        entry->will_message = MQTT_SERVER_NONE;
        entry->will_due_ms = 0U;
        if (source.retain && server->config.retain_available) {
            if (mqtt_session_retain(server, &source) != MQTT_OK) {
                server->statistics.messages_dropped += 1U;
            }
        }
        (void)mqtt_route_publish(server, &source);
        mqtt_route_release(server, source.message);
    }
}

void mqtt_session_discard(mqtt_server *server, uint32_t session)
{
    /** @brief The Session. */
    mqtt_session *entry = &server->storage.sessions[session];

    mqtt_session_publish_will(server, session);
    mqtt_journal_session_remove(server, session);
    while (entry->subscriptions != MQTT_SERVER_NONE) {
        session_subscription_free(server, entry->subscriptions);
    }
    session_free_deliveries(server, entry->queued_head);
    session_free_deliveries(server, entry->inflight_head);
    session_free_deliveries(server, entry->incoming_head);
    session_table_remove(server, session);
    mqtt_page_arena_give(&server->arena, entry->identifier_block);
    entry->in_use = false;
    (void)mqtt_slab_give(&server->session_slots, session);
}

/**
 * @brief Find the group of a Shared Subscription at a node, adding it if it is
 * missing.
 * @param[in,out] server The server.
 * @param[in] node The node.
 * @param[in] share_name The ShareName.
 * @return The group, or MQTT_SERVER_NONE if a pool is exhausted.
 */
static uint32_t session_group_find(mqtt_server *server, uint32_t node,
                                   const mqtt_utf8 *share_name)
{
    /** @brief The group being looked at. */
    uint32_t group = server->tree.nodes[node].groups;
    /** @brief true once found. */
    bool found = false;

    while ((!found) && (group != MQTT_SERVER_NONE)) {
        /** @brief The group. */
        const mqtt_share_group *entry = &server->storage.groups[group];

        found = (entry->name_length == share_name->length) &&
                mqtt_bytes_equal(
                    mqtt_page_arena_bytes(&server->arena, entry->name_block),
                    share_name->data, (uint32_t)share_name->length);
        if (!found) {
            group = entry->next;
        }
    }
    if ((!found) && (!mqtt_slab_full(&server->group_slots))) {
        /** @brief A block for the ShareName. */
        const uint32_t block =
            mqtt_page_arena_take(&server->arena, (uint32_t)share_name->length);

        if (block != MQTT_PAGE_ARENA_NONE) {
            /** @brief The node. */
            mqtt_tree_node *above = &server->tree.nodes[node];
            /** @brief The new group. */
            mqtt_share_group *entry = NULL;

            group = mqtt_slab_take(&server->group_slots);
            entry = &server->storage.groups[group];
            entry->name_block = block;
            entry->name_length = share_name->length;
            entry->members = MQTT_SERVER_NONE;
            entry->cursor = MQTT_SERVER_NONE;
            entry->previous = MQTT_SERVER_NONE;
            entry->next = above->groups;
            if (above->groups != MQTT_SERVER_NONE) {
                server->storage.groups[above->groups].previous = group;
            }
            above->groups = group;
            /* The block was taken for the name, so it fits. */
            mqtt_bytes_put(mqtt_page_arena_bytes(&server->arena, block),
                           share_name->data, (uint32_t)share_name->length);
        }
    }
    return group;
}

/**
 * @brief Find a Session's subscription at a node.
 * @param[in] server The server.
 * @param[in] session The Session.
 * @param[in] node The node.
 * @param[in] group The group, or MQTT_SERVER_NONE.
 * @return The subscription, or MQTT_SERVER_NONE.
 */
static uint32_t session_find_subscription(const mqtt_server *server,
                                          uint32_t session, uint32_t node,
                                          uint32_t group)
{
    /** @brief The subscription being looked at. */
    uint32_t index = server->storage.sessions[session].subscriptions;
    /** @brief The subscription found. */
    uint32_t found = MQTT_SERVER_NONE;

    while ((found == MQTT_SERVER_NONE) && (index != MQTT_SERVER_NONE)) {
        /** @brief The subscription. */
        const mqtt_subscription *item = &server->storage.subscriptions[index];

        if ((item->node == node) && (item->group == group)) {
            found = index;
        }
        index = item->next_of_session;
    }
    return found;
}

/**
 * @brief Take a new group, which is first at its node, off the node and give
 * it back to the pool.
 * @param[in,out] server The server.
 * @param[in] node The node.
 * @param[in] group The group.
 */
static void session_group_discard(mqtt_server *server, uint32_t node,
                                  uint32_t group)
{
    /** @brief The node. */
    mqtt_tree_node *above = &server->tree.nodes[node];
    /** @brief The group. */
    const mqtt_share_group *entry = &server->storage.groups[group];

    above->groups = entry->next;
    if (entry->next != MQTT_SERVER_NONE) {
        server->storage.groups[entry->next].previous = MQTT_SERVER_NONE;
    }
    mqtt_page_arena_give(&server->arena, entry->name_block);
    (void)mqtt_slab_give(&server->group_slots, group);
}

/**
 * @brief Add a subscription at the end of a group's members, so that members
 * get their turns in the order they joined.
 * @param[in,out] server The server.
 * @param[in] group The group.
 * @param[in] index The subscription.
 */
static void session_group_join(mqtt_server *server, uint32_t group,
                               uint32_t index)
{
    /** @brief The subscription. */
    mqtt_subscription *item = &server->storage.subscriptions[index];
    /** @brief The group. */
    mqtt_share_group *members = &server->storage.groups[group];
    /** @brief The last member so far. */
    uint32_t last = members->members;

    while ((last != MQTT_SERVER_NONE) &&
           (server->storage.subscriptions[last].next_here !=
            MQTT_SERVER_NONE)) {
        last = server->storage.subscriptions[last].next_here;
    }
    item->next_here = MQTT_SERVER_NONE;
    item->previous_here = last;
    if (last == MQTT_SERVER_NONE) {
        members->members = index;
    } else {
        server->storage.subscriptions[last].next_here = index;
    }
}

/**
 * @brief Add a subscription at the front of the ones at its node.
 * @param[in,out] server The server.
 * @param[in] node The node.
 * @param[in] index The subscription.
 */
static void session_node_join(mqtt_server *server, uint32_t node,
                              uint32_t index)
{
    /** @brief The subscription. */
    mqtt_subscription *item = &server->storage.subscriptions[index];
    /** @brief The node. */
    mqtt_tree_node *above = &server->tree.nodes[node];

    item->next_here = above->subscriptions;
    if (above->subscriptions != MQTT_SERVER_NONE) {
        /** @brief The subscription that was first here. */
        mqtt_subscription *first =
            &server->storage.subscriptions[above->subscriptions];

        first->previous_here = index;
    }
    above->subscriptions = index;
}

/**
 * @brief Take a subscription from the pool, which must not be empty, and add
 * it to a Session and to a node or a group.
 * @param[in,out] server The server.
 * @param[in] session The Session.
 * @param[in] node The node.
 * @param[in] group The group, or MQTT_SERVER_NONE.
 * @param[in] options The Subscription Options.
 * @param[in] identifier The Subscription Identifier, or zero.
 * @param[in] shared true if it is a Shared Subscription, which joins the group.
 */
static void session_subscription_add(mqtt_server *server, uint32_t session,
                                     uint32_t node, uint32_t group,
                                     uint8_t options, uint32_t identifier,
                                     bool shared)
{
    /** @brief Index of the new subscription, as one is free. */
    const uint32_t index = mqtt_slab_take(&server->subscription_slots);
    /** @brief The new subscription. */
    mqtt_subscription *item = &server->storage.subscriptions[index];
    /** @brief The Session. */
    mqtt_session *entry = &server->storage.sessions[session];

    item->node = node;
    item->session = session;
    item->group = group;
    item->identifier = identifier;
    item->options = options;
    item->previous_here = MQTT_SERVER_NONE;
    item->previous_of_session = MQTT_SERVER_NONE;
    item->next_of_session = entry->subscriptions;
    if (entry->subscriptions != MQTT_SERVER_NONE) {
        /** @brief The subscription that was first in the Session. */
        mqtt_subscription *first =
            &server->storage.subscriptions[entry->subscriptions];

        first->previous_of_session = index;
    }
    entry->subscriptions = index;
    if (shared) {
        session_group_join(server, group, index);
    } else {
        session_node_join(server, node, index);
    }
}

mqtt_status mqtt_session_subscribe(mqtt_server *server, uint32_t session,
                                   const mqtt_utf8 *filter, uint8_t options,
                                   uint32_t identifier, bool shared,
                                   bool *was_new)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief The filter to put in the tree. */
    mqtt_utf8 inner = *filter;
    /** @brief The ShareName. */
    mqtt_utf8 share_name = {0U, NULL};
    /** @brief The node. */
    uint32_t node = MQTT_TREE_NONE;
    /** @brief The group. */
    uint32_t group = MQTT_SERVER_NONE;

    *was_new = false;
    if (shared) {
        (void)mqtt_topic_share_split(filter, &share_name, &inner);
    }
    status = mqtt_tree_insert(&server->tree, &inner, &node);
    if ((status == MQTT_OK) && shared) {
        group = session_group_find(server, node, &share_name);
        if (group == MQTT_SERVER_NONE) {
            mqtt_tree_prune(&server->tree, node);
            status = MQTT_QUOTA_EXCEEDED;
        }
    }
    if (status == MQTT_OK) {
        /** @brief The subscription. */
        const uint32_t index =
            session_find_subscription(server, session, node, group);

        if (index != MQTT_SERVER_NONE) {
            server->storage.subscriptions[index].options = options;
            server->storage.subscriptions[index].identifier = identifier;
        } else if (mqtt_slab_full(&server->subscription_slots)) {
            /* A group with no members has to go too, and the node with it. */
            if (shared &&
                (server->storage.groups[group].members == MQTT_SERVER_NONE)) {
                session_group_discard(server, node, group);
            }
            mqtt_tree_prune(&server->tree, node);
            status = MQTT_QUOTA_EXCEEDED;
        } else {
            session_subscription_add(server, session, node, group, options,
                                     identifier, shared);
            *was_new = true;
        }
    }
    if (status == MQTT_OK) {
        mqtt_journal_subscribe(server, session, filter, options, identifier,
                               shared);
    }
    return status;
}

mqtt_status mqtt_session_unsubscribe(mqtt_server *server, uint32_t session,
                                     const mqtt_utf8 *filter, bool shared)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_NOT_FOUND;
    /** @brief The filter that is in the tree. */
    mqtt_utf8 inner = *filter;
    /** @brief The ShareName. */
    mqtt_utf8 share_name = {0U, NULL};

    if (shared) {
        (void)mqtt_topic_share_split(filter, &share_name, &inner);
    }
    {
        /** @brief The node. */
        const uint32_t node = mqtt_tree_find(&server->tree, &inner);

        if (node != MQTT_TREE_NONE) {
            /** @brief The group being looked at. */
            uint32_t group =
                shared ? server->tree.nodes[node].groups : MQTT_SERVER_NONE;
            /** @brief true once the search of groups is over. */
            bool done = !shared;

            while ((!done) && (group != MQTT_SERVER_NONE)) {
                /** @brief The group. */
                const mqtt_share_group *entry = &server->storage.groups[group];

                done = (entry->name_length == share_name.length) &&
                       mqtt_bytes_equal(mqtt_page_arena_bytes(
                                            &server->arena, entry->name_block),
                                        share_name.data,
                                        (uint32_t)share_name.length);
                if (!done) {
                    group = entry->next;
                }
            }
            if (done) {
                /** @brief The subscription. */
                const uint32_t index =
                    session_find_subscription(server, session, node, group);

                if (index != MQTT_SERVER_NONE) {
                    session_subscription_free(server, index);
                    mqtt_journal_unsubscribe(server, session, filter, shared);
                    status = MQTT_OK;
                }
            }
        }
    }
    return status;
}

void mqtt_session_send_retained(mqtt_server *server, uint32_t session,
                                const mqtt_utf8 *filter, uint8_t qos,
                                uint32_t identifier)
{
    /** @brief The state of the walk. */
    mqtt_tree_match cursor;
    /** @brief The node found. */
    uint32_t node = MQTT_TREE_NONE;

    mqtt_tree_walk_begin(&cursor, filter);
    if (server->storage.sessions[session].version == MQTT_VERSION_31) {
        mqtt_tree_walk_reach_dollar(&cursor);
    }
    node = mqtt_tree_walk_next(&server->tree, &cursor);
    while (node != MQTT_TREE_NONE) {
        /** @brief The retained message here. */
        const uint32_t message = server->tree.nodes[node].retained;

        if (message != MQTT_SERVER_NONE) {
            /* One that has expired stays for the timers, which can also remove
             * its node. */
            if (!mqtt_route_expired(server, message)) {
                /** @brief The message's QoS. */
                const uint8_t published = server->storage.messages[message].qos;

                (void)mqtt_route_enqueue(server, session, message,
                                         (published < qos) ? published : qos,
                                         true, identifier);
            }
        }
        node = mqtt_tree_walk_next(&server->tree, &cursor);
    }
}

mqtt_status mqtt_session_retain(mqtt_server *server, mqtt_route_source *source)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (source->payload.length == 0U) {
        /** @brief The node of this topic. */
        const uint32_t node =
            mqtt_tree_find(&server->tree, &source->topic_name);

        if ((node != MQTT_TREE_NONE) &&
            (server->tree.nodes[node].retained != MQTT_SERVER_NONE)) {
            mqtt_journal_retained_remove(server, &source->topic_name);
            mqtt_route_release(server, server->tree.nodes[node].retained);
            server->tree.nodes[node].retained = MQTT_SERVER_NONE;
            mqtt_tree_prune(&server->tree, node);
        }
    } else {
        /** @brief The node of this topic. */
        uint32_t node = MQTT_TREE_NONE;

        status = mqtt_tree_insert(&server->tree, &source->topic_name, &node);
        if (status == MQTT_OK) {
            if (source->message == MQTT_SERVER_NONE) {
                source->message = mqtt_route_store(server, source);
            }
            if (source->message == MQTT_SERVER_NONE) {
                /* The old one goes all the same, as this one replaces it:
                 * [MQTT-3.3.1-5], -7. */
                mqtt_journal_retained_remove(server, &source->topic_name);
                mqtt_route_release(server, server->tree.nodes[node].retained);
                server->tree.nodes[node].retained = MQTT_SERVER_NONE;
                mqtt_tree_prune(&server->tree, node);
                status = MQTT_QUOTA_EXCEEDED;
            } else {
                /** @brief The retained message it replaces. */
                const uint32_t old = server->tree.nodes[node].retained;

                /* The new one is written first, so the store never holds a
                 * message that is gone as retained. */
                mqtt_route_hold(server, source->message);
                server->tree.nodes[node].retained = source->message;
                mqtt_journal_retained(server, source->message);
                mqtt_route_release(server, old);
            }
        }
    }
    return status;
}
