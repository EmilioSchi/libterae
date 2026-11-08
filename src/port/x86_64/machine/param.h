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

/*
 * Stands in for FreeBSD's <machine/param.h>, which memcmp.S includes. It is
 * libterae's own, and gives only what memcmp.S uses: PAGE_SIZE, the smallest
 * page size on x86_64, 4096 bytes.
 *
 * Only memcmp's SSE2 variant reads it, to tell whether a 16-byte load would
 * cross into the next page. That variant isn't the one port_memcmp.S
 * selects, but it is still assembled, so the macro has to exist.
 */
#ifndef MQTT_PORT_MACHINE_PARAM_H
#define MQTT_PORT_MACHINE_PARAM_H

#define PAGE_SIZE 4096

#endif /* MQTT_PORT_MACHINE_PARAM_H */
