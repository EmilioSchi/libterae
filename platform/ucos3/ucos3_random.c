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
 * @file ucos3_random.c
 * @brief Random bytes, from the board.
 *
 * uC/OS-III and uC/TCP-IP give no random bytes that an attacker can't predict:
 * uC/LIB's Math_Rand is a linear congruential generator. So they come from
 * ucos3_board_random, which the application defines, as it does
 * ucos3_board_write.
 */

#include "base_platform.h"
#include "ucos3_internal.h"

mqtt_status mqtt_platform_random_bytes(uint8_t *data, uint32_t length)
{
    return ucos3_board_random(data, length) ? MQTT_OK : MQTT_INVALID_STATE;
}
