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
 * @file base_library.h
 * @brief The version of the library, and what a program checks it with.
 *
 * The version is three numbers, major.minor.patch, each from 0 to 255. The
 * major number changes when a program built for the version before may need
 * changing, or building again, to work with this one. The minor number
 * changes when the library gains something and a program built for an earlier
 * minor number still works with it. The patch number changes when nothing a
 * program can see changes. The Makefile reads these three definitions, and
 * names a shared library by them.
 *
 * The page size is part of what a program and its library share, as the
 * program sizes arrays with MQTT_PAGE_SIZE and the library fills them. A
 * program linked with an archive is built with it, but a shared library is
 * found when the program starts, and may have been built for another version
 * or with another page size. The functions here say what the library was
 * built as, so that a program can refuse one that doesn't match.
 */
#ifndef MQTT_BASE_LIBRARY_H
#define MQTT_BASE_LIBRARY_H

#include <stdbool.h>
#include <stdint.h>

/** @brief The major version of the library. */
#define MQTT_LIBRARY_VERSION_MAJOR 1U
/** @brief The minor version of the library. */
#define MQTT_LIBRARY_VERSION_MINOR 0U
/** @brief The patch version of the library. */
#define MQTT_LIBRARY_VERSION_PATCH 5U

#if MQTT_LIBRARY_VERSION_MAJOR > 255U
#error "MQTT_LIBRARY_VERSION_MAJOR must be from 0 to 255"
#endif
#if MQTT_LIBRARY_VERSION_MINOR > 255U
#error "MQTT_LIBRARY_VERSION_MINOR must be from 0 to 255"
#endif
#if MQTT_LIBRARY_VERSION_PATCH > 255U
#error "MQTT_LIBRARY_VERSION_PATCH must be from 0 to 255"
#endif

/**
 * @brief The version as one number, a byte for each part from the major down:
 * 0x010005 for 1.0.5. A later version has a larger number.
 */
#define MQTT_LIBRARY_VERSION                                                \
    ((MQTT_LIBRARY_VERSION_MAJOR * 65536U) +                                \
     (MQTT_LIBRARY_VERSION_MINOR * 256U) + MQTT_LIBRARY_VERSION_PATCH)

/**
 * @brief Find the version the library was built as.
 * @return Its MQTT_LIBRARY_VERSION.
 */
uint32_t mqtt_library_version(void);

/**
 * @brief Find the page size the library was built with.
 * @return Its MQTT_PAGE_SIZE.
 */
uint32_t mqtt_library_page_size(void);

/**
 * @brief Find whether a program built with headers of a version and a page
 * size can use the library: it has the same major version, a minor version
 * at least as high, and the same page size. The patch versions don't matter.
 * @param[in] version The MQTT_LIBRARY_VERSION the program was built with.
 * @param[in] page_size The MQTT_PAGE_SIZE the program was built with.
 * @return true if it can.
 */
bool mqtt_library_compatible(uint32_t version, uint32_t page_size);

#endif /* MQTT_BASE_LIBRARY_H */
