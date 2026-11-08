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
 * @file platform_probe_none.c
 * @brief The calls of the tests and the benchmarks that a system without
 * processes to copy or signals to interrupt with can't make.
 *
 * Each says so through what it returns, and the tests leave out what they
 * can't check.
 */

#include "base_platform.h"

bool mqtt_platform_interrupt_after_s(uint32_t seconds)
{
    (void)seconds;
    return false;
}

mqtt_status mqtt_platform_fork(bool *child)
{
    *child = false;
    return MQTT_INVALID_STATE;
}

void mqtt_platform_child_exit(int32_t code)
{
    (void)code;
}
