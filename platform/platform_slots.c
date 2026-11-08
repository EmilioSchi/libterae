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
 * @file platform_slots.c
 * @brief The indexes of a table of handles, given out from a free list.
 */

#include "platform_slots.h"

/**
 * @brief Put every index on the list, lowest first.
 * @param[out] slots The list.
 */
static void slots_make(platform_slots *slots)
{
    /** @brief Index of the entry. */
    uint32_t index = 0U;

    while (index < MQTT_PLATFORM_SOCKETS) {
        slots->next[index] = ((index + 1U) < MQTT_PLATFORM_SOCKETS) ?
                                 (int32_t)(index + 1U) :
                                 MQTT_SOCKET_NONE;
        index += 1U;
    }
    slots->head = 0;
    slots->ready = true;
}

int32_t platform_slots_take(platform_slots *slots)
{
    /** @brief The index taken. */
    int32_t taken = MQTT_SOCKET_NONE;

    if (!slots->ready) {
        slots_make(slots);
    }
    if (slots->head != MQTT_SOCKET_NONE) {
        taken = slots->head;
        slots->head = slots->next[taken];
        slots->next[taken] = MQTT_SOCKET_NONE;
    }
    return taken;
}

void platform_slots_give(platform_slots *slots, int32_t index)
{
    if (slots->ready && (index >= 0) &&
        ((uint32_t)index < MQTT_PLATFORM_SOCKETS)) {
        slots->next[index] = slots->head;
        slots->head = index;
    }
}
