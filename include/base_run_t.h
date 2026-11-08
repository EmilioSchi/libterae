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
 * @file base_run_t.h
 * @brief A run of bytes that are next to each other in memory.
 *
 * Bytes can be in more than one place: the blocks of a ring, or a payload sent
 * straight from the arena. A list of runs says where each part is, so that one
 * system call can send them all, in order, and a payload that arrived in
 * several blocks can be passed on without being copied into one.
 */
#ifndef MQTT_BASE_RUN_T_H
#define MQTT_BASE_RUN_T_H

#include <stdint.h>

/** @brief A run of bytes. It does not own them. */
typedef struct {
    /** @brief The first byte. */
    const uint8_t *data;
    /** @brief Number of bytes. */
    uint32_t length;
} mqtt_run;

/** @brief Room for bytes to be written. It does not own them. */
typedef struct {
    /** @brief The first byte. */
    uint8_t *data;
    /** @brief Number of bytes. */
    uint32_t length;
} mqtt_space;

/** @brief Bytes in several runs, in order. */
typedef struct {
    /** @brief The runs. */
    const mqtt_run *runs;
    /** @brief Number of runs. */
    uint32_t count;
    /** @brief Number of bytes in all of them. */
    uint32_t length;
} mqtt_run_list;

#endif /* MQTT_BASE_RUN_T_H */
