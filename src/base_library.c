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
 * @file base_library.c
 * @brief The version of the library and the page size it was built with.
 */

#include "base_library.h"
#include "base_page.h"

uint32_t mqtt_library_version(void)
{
    return MQTT_LIBRARY_VERSION;
}

uint32_t mqtt_library_page_size(void)
{
    return MQTT_PAGE_SIZE;
}

bool mqtt_library_compatible(uint32_t version, uint32_t page_size)
{
    /** @brief The program's major version, with any bits above it. */
    const uint32_t major = version / 65536U;
    /**
     * @brief The program's major and minor versions as one number, which is
     * no larger than the library's when the major versions are the same and
     * the minor version is no later.
     */
    const uint32_t major_minor = version / 256U;

    return (major == MQTT_LIBRARY_VERSION_MAJOR) &&
           (major_minor <= (MQTT_LIBRARY_VERSION / 256U)) &&
           (page_size == MQTT_PAGE_SIZE);
}
