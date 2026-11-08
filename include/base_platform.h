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
 * @file base_platform.h
 * @brief What the library needs from the operating system.
 *
 * The protocol code in src/ uses nothing outside standard C. Everything that
 * depends on the operating system is behind this interface, and the
 * implementation is in platform/.
 *
 * Sockets are waited for in the tiers of base_tier.h, with one call to poll for
 * each wait, which covers only the tiers that are due. A COLD socket loses
 * nothing: what arrives waits in the system's buffer, and is read at most
 * cold_period_ms later than it would be on a HOT socket.
 *
 * A store keeps a server's or a client's Session state across a restart: a
 * folder of files on a system that has them, a region of flash on a board. It
 * holds the snapshot in two or more copies and the journal in two places, used
 * in turn by generation. Records are written as they come, and a sync makes
 * them outlast a loss of power when a COMMIT asks for one. What the bytes mean
 * is the library's: this only keeps them.
 *
 * The last few calls are there for the tests and the benchmarks: a pair of
 * connected sockets, the processor time used, a signal that interrupts a wait,
 * and a second process. A system without some of them says so through what
 * they return.
 */
#ifndef MQTT_BASE_PLATFORM_H
#define MQTT_BASE_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

#include "base_run_t.h"
#include "base_status.h"
#include "base_store.h"
#include "base_tier.h"

/** @brief The value of a socket that is not open. */
#define MQTT_SOCKET_NONE (-1)
/** @brief The value of a store that is not open. */
#define MQTT_STORE_NONE (-1)
/** @brief The fewest snapshot copies a store keeps. */
#define MQTT_PLATFORM_STORE_COPIES_MIN 2U
/** @brief The most snapshot copies a store keeps. */
#define MQTT_PLATFORM_STORE_COPIES_MAX 8U
/** @brief The most stores open at one time: a server's and a client's. */
#define MQTT_PLATFORM_STORES 2U
#if !defined(MQTT_PLATFORM_WAIT_SOCKETS)
/**
 * @brief The most sockets that can be waited for at one time, unless the build
 * sets another number from 8 to 1048576, as a small system may.
 */
#define MQTT_PLATFORM_WAIT_SOCKETS 65536U
#endif

#if (MQTT_PLATFORM_WAIT_SOCKETS < 8U) || (MQTT_PLATFORM_WAIT_SOCKETS > 1048576U)
#error "MQTT_PLATFORM_WAIT_SOCKETS must be from 8 to 1048576"
#endif
/**
 * @brief The most runs one write takes, which is the least any POSIX system
 * allows.
 */
#define MQTT_PLATFORM_WRITE_RUNS 16U
/**
 * @brief The most places one read fills, which is also the fewest POSIX lets
 * a system allow.
 */
#define MQTT_PLATFORM_READ_SPACES 16U

/** @brief A socket found ready by a wait. */
typedef struct {
    /** @brief What the socket was added with. */
    uint32_t tag;
    /** @brief Its handle in the tier set. */
    uint32_t handle;
    /** @brief It can be read, or it has failed. */
    bool readable;
    /** @brief It can be written. */
    bool writable;
    /** @brief The other end has gone, or the socket has failed. */
    bool hang_up;
} mqtt_wait_event;

/**
 * @brief Write text to the program's diagnostic output.
 * @param[in] text The text, NUL-terminated.
 */
void mqtt_platform_console_write(const char *text);

/**
 * @brief Write text to the program's normal output.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes in data.
 */
void mqtt_platform_output_write(const uint8_t *data, uint32_t length);

/**
 * @brief Read the clock.
 * @return Milliseconds since an arbitrary point. It only goes forward.
 */
uint64_t mqtt_platform_time_ms(void);

/**
 * @brief Find how many bytes a page of the system's memory holds, to compare
 * with the MQTT_PAGE_SIZE the library was built with.
 * @return Number of bytes, or zero if the system doesn't say.
 */
uint32_t mqtt_platform_page_size(void);

/**
 * @brief Read the clock, more finely.
 * @return Microseconds since the same point as mqtt_platform_time_ms.
 */
uint64_t mqtt_platform_time_us(void);

/**
 * @brief Fill bytes from the system's source of random numbers, which an
 * attacker can't predict.
 * @param[out] data Where to put them.
 * @param[in] length Number of bytes.
 * @return MQTT_OK, or MQTT_INVALID_STATE if the system has no such source or it
 * failed, and then what data holds means nothing.
 */
mqtt_status mqtt_platform_random_bytes(uint8_t *data, uint32_t length);

/**
 * @brief Open a TCP socket that accepts connections.
 * @param[in] host The local address to bind, or NULL for all of them.
 * @param[in] port The local port, as text.
 * @param[out] listener The socket.
 * @return MQTT_OK or MQTT_CONNECTION_CLOSED.
 */
mqtt_status mqtt_platform_listen(const char *host, const char *port,
                                 int32_t *listener);

/**
 * @brief Set the size of a socket's send buffer, which the system may round.
 * @param[in] handle The socket.
 * @param[in] bytes Number of bytes.
 */
void mqtt_platform_send_buffer(int32_t handle, uint32_t bytes);

/**
 * @brief Accept a connection, if one is waiting.
 * @param[in] listener The listening socket.
 * @param[out] handle The new socket, which does not block.
 * @return MQTT_OK, MQTT_INCOMPLETE if none is waiting, or
 * MQTT_CONNECTION_CLOSED.
 */
mqtt_status mqtt_platform_accept(int32_t listener, int32_t *handle);

/**
 * @brief Open a TCP connection and wait for it to complete.
 * @param[in] host The host to connect to.
 * @param[in] port The port, as text.
 * @param[out] handle The new socket, which does not block.
 * @return MQTT_OK or MQTT_CONNECTION_CLOSED.
 */
mqtt_status mqtt_platform_connect(const char *host, const char *port,
                                  int32_t *handle);

/**
 * @brief Read from a socket into several places, in order, with one system
 * call.
 * @param[in] handle The socket.
 * @param[in] spaces The places. No more than MQTT_PLATFORM_READ_SPACES are
 * used, and fewer where the system takes fewer.
 * @param[in] count Number of places.
 * @param[out] count_read Number of bytes read, on MQTT_OK.
 * @return MQTT_OK, MQTT_INCOMPLETE if nothing is waiting, or
 * MQTT_CONNECTION_CLOSED.
 */
mqtt_status mqtt_platform_read_spaces(int32_t handle, const mqtt_space *spaces,
                                      uint32_t count, uint32_t *count_read);

/**
 * @brief Read from a socket.
 * @param[in] handle The socket.
 * @param[out] data Where to put the bytes.
 * @param[in] capacity Number of bytes data can hold.
 * @param[out] count Number of bytes read, on MQTT_OK.
 * @return MQTT_OK, MQTT_INCOMPLETE if nothing is waiting, or
 * MQTT_CONNECTION_CLOSED.
 */
mqtt_status mqtt_platform_read(int32_t handle, uint8_t *data, uint32_t capacity,
                               uint32_t *count);

/**
 * @brief Write to a socket.
 * @param[in] handle The socket.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes in data.
 * @param[out] count Number of bytes taken, on MQTT_OK.
 * @return MQTT_OK, MQTT_INCOMPLETE if nothing could be taken, or
 * MQTT_CONNECTION_CLOSED.
 */
mqtt_status mqtt_platform_write(int32_t handle, const uint8_t *data,
                                uint32_t length, uint32_t *count);

/**
 * @brief Write runs of bytes to a socket, in order, with one system call.
 * @param[in] handle The socket.
 * @param[in] runs The runs.
 * @param[in] count Number of runs. Only the first MQTT_PLATFORM_WRITE_RUNS are
 * written.
 * @param[out] count_taken Number of bytes taken, on MQTT_OK. Fewer than were
 * given means the socket is full.
 * @return MQTT_OK, MQTT_INCOMPLETE if nothing could be taken, or
 * MQTT_CONNECTION_CLOSED.
 */
mqtt_status mqtt_platform_write_runs(int32_t handle, const mqtt_run *runs,
                                     uint32_t count, uint32_t *count_taken);

/**
 * @brief Close a socket.
 * @param[in] handle The socket.
 */
void mqtt_platform_close(int32_t handle);

/**
 * @brief Sleep.
 * @param[in] milliseconds How long.
 */
void mqtt_platform_sleep_ms(uint32_t milliseconds);

/**
 * @brief Find out whether the program has been asked to stop, by a signal or
 * by mqtt_platform_request_stop.
 * @return true if it has.
 */
bool mqtt_platform_stop_requested(void);

/**
 * @brief Ask the program to stop, as a signal would, for a system with no
 * signals or a task that ends the others: from then on
 * mqtt_platform_stop_requested says so, and the wake handle is readable.
 */
void mqtt_platform_request_stop(void);

/**
 * @brief Set up waiting, with no socket in it yet.
 * @param[in] config How the tiers behave, with max_sockets at most
 * MQTT_PLATFORM_WAIT_SOCKETS, or NULL for the defaults.
 * @return MQTT_OK or MQTT_INVALID_ARGUMENT.
 */
mqtt_status mqtt_platform_wait_start(const mqtt_tier_config *config);

/**
 * @brief Start waiting for a socket to be readable. It starts HOT.
 * @param[in] socket The socket.
 * @param[in] tag What to report it with.
 * @param[in] pins Reasons it has to stay HOT, MQTT_TIER_PIN_ bits.
 * @return Its handle, or MQTT_TIER_NONE if no more can be waited for.
 */
uint32_t mqtt_platform_wait_add(int32_t socket, uint32_t tag, uint32_t pins);

/**
 * @brief Say what to wait for on a socket. Waiting to write makes it HOT and
 * pins it MQTT_TIER_PIN_TX_PENDING until this is called with write false.
 * @param[in] handle The socket's handle.
 * @param[in] read Whether to wait for read readiness.
 * @param[in] write Whether to wait for write readiness.
 */
void mqtt_platform_wait_interest(uint32_t handle, bool read, bool write);

/**
 * @brief Stop waiting for a socket. The socket stays open.
 * @param[in] handle The socket's handle.
 */
void mqtt_platform_wait_remove(uint32_t handle);

/**
 * @brief Find the tier set, to report reads, pins and Keep Alive to it, and to
 * read its counts. A change of tier through it keeps the waits in step.
 * @return The tier set.
 */
mqtt_tier *mqtt_platform_wait_tier(void);

/**
 * @brief Wait for the sockets that are due, with one call to poll.
 *
 * A signal does not end the wait early: it goes on until the time given. A
 * socket that is reported ready and not acted on is reported again by the next
 * wait that covers it.
 *
 * @param[out] events The sockets found ready.
 * @param[in] capacity Number of entries events can hold.
 * @param[in] max_wait_ms The longest the wait may take, in milliseconds.
 * @return Number of events.
 */
uint32_t mqtt_platform_wait(mqtt_wait_event *events, uint32_t capacity,
                            uint32_t max_wait_ms);

/**
 * @brief Wait for one socket. Nothing is kept from one call to the next, so
 * any task may call it, beside the waits of a server.
 *
 * A socket that has failed, or whose other end has gone, is reported as
 * readable and hung up when read readiness is asked for. With nothing asked
 * for, only some systems report it. A signal may end the wait early.
 *
 * @param[in] socket The socket.
 * @param[in] read Whether to wait for read readiness.
 * @param[in] write Whether to wait for write readiness.
 * @param[in] max_wait_ms The longest the wait may take, in milliseconds.
 * @param[out] event What came, in readable, writable and hang_up, which are
 * all false if nothing did. Its tag and handle are zero.
 * @return true if anything came.
 */
bool mqtt_platform_wait_socket(int32_t socket, bool read, bool write,
                               uint32_t max_wait_ms, mqtt_wait_event *event);

/**
 * @brief Find the socket that becomes readable when a signal asks the program
 * to stop, so a wait can end at once.
 * @return The socket, or MQTT_SOCKET_NONE if there is none.
 */
int32_t mqtt_platform_wake_handle(void);

/** @brief Take what the stop signals wrote to that socket. */
void mqtt_platform_wake_drain(void);

/**
 * @brief Open two sockets connected to each other, which do not block.
 * @param[out] first One end.
 * @param[out] second The other.
 * @return MQTT_OK or MQTT_CONNECTION_CLOSED.
 */
mqtt_status mqtt_platform_socket_pair(int32_t *first, int32_t *second);

/**
 * @brief Tell whether closing a connection whose socket holds bytes it hasn't
 * read resets it, as a POSIX close does, so that the peer finds out at once,
 * even while it has bytes of its own to send.
 * @return true if it does, or false on a system whose stack only ends the
 * connection: a peer that still has bytes to send may then find out only once
 * they drain, or when a keep-alive fails.
 */
bool mqtt_platform_close_resets(void);

/**
 * @brief Read the processor time this process has used.
 * @return Microseconds, or zero if the system doesn't say.
 */
uint64_t mqtt_platform_cpu_us(void);

/**
 * @brief Have a signal arrive after some time, interrupting whatever system
 * call is going on then.
 * @param[in] seconds How long from now.
 * @return true if it will, or false on a system with no such signal.
 */
bool mqtt_platform_interrupt_after_s(uint32_t seconds);

/**
 * @brief Start a second process, a copy of this one.
 * @param[out] child true in the new process, and false in this one.
 * @return MQTT_OK, or MQTT_INVALID_STATE if no process could be started, as on
 * a system that can't copy a process.
 */
mqtt_status mqtt_platform_fork(bool *child);

/**
 * @brief End a process that mqtt_platform_fork started.
 * @param[in] code What it returns.
 */
void mqtt_platform_child_exit(int32_t code);

/**
 * @brief Convert a decimal string to a number.
 * @param[in] text The text.
 * @param[out] value The number, if the text was valid.
 * @return true if the text is a decimal number that fits in 32 bits.
 */
bool mqtt_platform_parse_u32(const char *text, uint32_t *value);

/**
 * @brief Convert the name of a durability to its value: memory, written or
 * synced.
 * @param[in] text The name.
 * @param[out] durability The value, if the name is one of those.
 * @return true if it is.
 */
bool mqtt_platform_parse_durability(const char *text,
                                    mqtt_store_durability *durability);

/**
 * @brief Open a store, making it if there is none.
 * @param[in] where The folder of its files, or the name of the board's region.
 * @param[in] copies Number of snapshot copies, from
 * MQTT_PLATFORM_STORE_COPIES_MIN to MQTT_PLATFORM_STORE_COPIES_MAX.
 * @param[out] store The store.
 * @return MQTT_OK, MQTT_INVALID_ARGUMENT, MQTT_QUOTA_EXCEEDED if
 * MQTT_PLATFORM_STORES are open, or MQTT_INVALID_STATE if the medium can't be
 * opened, or the system has none.
 */
mqtt_status mqtt_platform_store_open(const char *where, uint32_t copies,
                                     int32_t *store);

/**
 * @brief Close a store.
 * @param[in] store The store. One not open is ignored.
 */
void mqtt_platform_store_close(int32_t store);

/**
 * @brief Find how many snapshot copies a store keeps.
 * @param[in] store The store.
 * @return Number of copies, or 0 for a store not open.
 */
uint32_t mqtt_platform_store_copies(int32_t store);

/**
 * @brief Add a record to the journal place in use, as a server's or a client's
 * store_append. Nothing is synced yet.
 * @param[in] user_tag The store.
 * @param[in] record The record.
 * @return MQTT_OK, or MQTT_INVALID_STATE if it could not be written whole, when
 * what was written of it is taken back.
 */
mqtt_status mqtt_platform_store_append(uint32_t user_tag,
                                       const mqtt_store_record *record);

/**
 * @brief Sync the journal, if a COMMIT added since the last call asked for it.
 * @param[in] store The store.
 * @return MQTT_OK, or MQTT_INVALID_STATE if the sync failed.
 */
mqtt_status mqtt_platform_store_commit(int32_t store);

/**
 * @brief Read bytes of a place of a store, as mqtt_store_source's read.
 * @param[in] user_tag The store.
 * @param[in] part The part.
 * @param[in] place The copy, or the journal place.
 * @param[in] offset Offset of the first byte.
 * @param[out] data Where the bytes go.
 * @param[in] capacity Number of bytes wanted.
 * @param[out] count Number of bytes read, fewer only at the end of the place.
 * @return MQTT_OK, or MQTT_INVALID_STATE if the medium could not be read.
 */
mqtt_status mqtt_platform_store_read(uint32_t user_tag, mqtt_store_part part,
                                     uint32_t place, uint64_t offset,
                                     uint8_t *data, uint32_t capacity,
                                     uint32_t *count);

/**
 * @brief Start a journal segment: empty the place its generation takes, write
 * its header, sync, and add records there from now on.
 * @param[in] store The store.
 * @param[in] header The MQTT_STORE_HEADER_SIZE bytes of its header.
 * @return MQTT_OK, or MQTT_INVALID_STATE if the medium failed.
 */
mqtt_status mqtt_platform_store_segment(int32_t store, const uint8_t *header);

/**
 * @brief Start a snapshot in the first copy: empty it and write its header.
 * @param[in] store The store.
 * @param[in] header The MQTT_STORE_HEADER_SIZE bytes of its header.
 * @return MQTT_OK, or MQTT_INVALID_STATE if the medium failed.
 */
mqtt_status mqtt_platform_store_copy_begin(int32_t store,
                                           const uint8_t *header);

/**
 * @brief Add a record of the snapshot to the first copy.
 * @param[in] store The store.
 * @param[in] record The record.
 * @return MQTT_OK, or MQTT_INVALID_STATE if the medium failed or is full.
 */
mqtt_status mqtt_platform_store_copy_record(int32_t store,
                                            const mqtt_store_record *record);

/**
 * @brief Finish the first copy of a snapshot: sync it.
 * @param[in] store The store.
 * @return MQTT_OK, or MQTT_INVALID_STATE if the medium failed.
 */
mqtt_status mqtt_platform_store_copy_end(int32_t store);

/**
 * @brief Write each other copy of a snapshot from the first, one at a time,
 * each synced before the next begins. The first copy is left as it is, so this
 * can be asked again after it fails.
 * @param[in] store The store.
 * @return MQTT_OK, or MQTT_INVALID_STATE if the medium failed.
 */
mqtt_status mqtt_platform_store_copy_duplicate(int32_t store);

/**
 * @brief Put everything a store holds aside, under a name of its own, and
 * start it again empty. Nothing is deleted.
 * @param[in] store The store.
 * @param[out] aside The number of the place it went to.
 * @return MQTT_OK, or MQTT_INVALID_STATE if it could not be moved.
 */
mqtt_status mqtt_platform_store_set_aside(int32_t store, uint32_t *aside);

/**
 * @brief Find the value of a byte of the store that was never written, as a
 * restore needs: 0x00 in a file, 0xFF in flash.
 * @param[in] store The store.
 * @return The value.
 */
uint8_t mqtt_platform_store_erased(int32_t store);

/**
 * @brief Find how many bytes each place of a store holds at most, so that a
 * snapshot is due before a journal segment fills its place.
 * @param[in] store The store.
 * @return Number of bytes, UINT64_MAX if only the medium's own size limits
 * them, or 0 for a store not open.
 */
uint64_t mqtt_platform_store_capacity(int32_t store);

/**
 * @brief Find how long ago the journal was last written, as a restore needs.
 * @param[in] store The store.
 * @return Milliseconds, or 0 if the medium can't tell.
 */
uint64_t mqtt_platform_store_downtime(int32_t store);

/**
 * @brief Remove a store that isn't open, with every place it set aside, for
 * the tests, which make stores of their own. A program never removes one: it
 * sets one aside.
 * @param[in] where The folder of its files, or the name of the board's region.
 * @return MQTT_OK, MQTT_NOT_FOUND if there is no such store, or
 * MQTT_INVALID_STATE if part of it could not be removed or the system has no
 * stores.
 */
mqtt_status mqtt_platform_store_remove(const char *where);

#endif /* MQTT_BASE_PLATFORM_H */
