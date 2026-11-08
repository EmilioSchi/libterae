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
 * @file base_store.h
 * @brief The format of stored Session state: segments, records and checks.
 *
 * What a server or a client keeps across a restart is written as records. A
 * snapshot holds the whole state, and a journal holds each change made after
 * it. Each snapshot copy and each journal segment is a place of its own, and
 * starts with a header that names the format, the kind of segment and its
 * generation. The library owns these bytes, and the application or the
 * platform owns the medium they are kept on.
 *
 * Integers are in network byte order, as in MQTT, and the header is written
 * field by field, so it reads the same on every processor. A CRC-16 covers the
 * header and each record. It finds accidental damage, such as a write cut
 * short, but not a forgery, which only the protection of the medium can stop.
 *
 * A journal is a series of groups, each ending with a COMMIT record. A crash
 * can cut the last record short. Reading takes everything up to the last
 * COMMIT and leaves the rest, so a change is kept whole or not at all. Bytes
 * after a record that fails its check have to be all the erased value of the
 * medium for the record to count as cut short, and otherwise the segment is
 * damaged.
 *
 * A record's kind and length have a CRC of their own, which is checked before
 * the length is trusted. Without it, a length damaged so that the record seems
 * to go past the end of its place would look like a record a crash cut short,
 * and it and every group after it would be left out with no fault.
 *
 * [V31] MQTT V3.1 Protocol Specification.
 * @li Cited: 2.2, 3.3, 4.1.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015.
 * @li Cited: 1.5, 3.1.2.4, 4.1.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 1.5, p.17-20. 4.1, p.92-93. 5.4.5, p.113.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */
#ifndef MQTT_BASE_STORE_H
#define MQTT_BASE_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "base_run_t.h"
#include "base_status.h"

/** @brief Number of bytes in a segment header. */
#define MQTT_STORE_HEADER_SIZE 58U

/**
 * @brief The major version of the format this library reads and writes. The
 * second gave each record's kind and length a CRC of their own.
 */
#define MQTT_STORE_FORMAT_MAJOR 2U

/** @brief The minor version of the format this library writes. */
#define MQTT_STORE_FORMAT_MINOR 0U

/**
 * @brief Number of reserved bytes in a segment header, kept for fields a later
 * minor version adds.
 */
#define MQTT_STORE_RESERVED_SIZE 32U

/**
 * @brief Number of bytes before a record's content: its kind (1), its length
 * (4), then the CRC-16 of those five (2).
 */
#define MQTT_STORE_FRAME_HEAD 7U

/** @brief Number of bytes after a record's content: its CRC-16. */
#define MQTT_STORE_FRAME_TAIL 2U

/** @brief The value a CRC-16 starts from. */
#define MQTT_STORE_CRC_INIT 0xFFFFU

/**
 * @brief The largest content a record may have: a whole MQTT packet, with
 * room for the fields stored with it.
 */
#define MQTT_STORE_CONTENT_MAX 0x10000040U

/** @brief Header flag of a journal segment that follows a gap. */
#define MQTT_STORE_FLAG_RECOVERY 0x01U

/** @brief Kind of segment: a snapshot of a server. */
#define MQTT_STORE_SERVER_SNAPSHOT 1U

/** @brief Kind of segment: a journal of a server. */
#define MQTT_STORE_SERVER_JOURNAL 2U

/** @brief Kind of segment: a snapshot of a client. */
#define MQTT_STORE_CLIENT_SNAPSHOT 3U

/** @brief Kind of segment: a journal of a client. */
#define MQTT_STORE_CLIENT_JOURNAL 4U

/**
 * @brief Kind of record that ends a snapshot. Its content is the number of
 * records before it, then the next store id, 8 bytes each.
 */
#define MQTT_STORE_RECORD_END 0x7EU

/**
 * @brief Kind of record that ends a group in a journal. Its content is the
 * milliseconds since the segment began, in 8 bytes.
 */
#define MQTT_STORE_RECORD_COMMIT 0x7FU

/**
 * @brief Bit of a record kind that a reader may skip if it doesn't know the
 * kind. A later minor version may add such records, and this one writes none.
 */
#define MQTT_STORE_RECORD_OPTIONAL 0x80U

/**
 * @brief Kind of record: a Session, which lasts after its connection. Its
 * content is the protocol version (1 byte), flags (1), the Session Expiry
 * Interval in seconds (4), the milliseconds left before the Session ends and
 * before its Will Message is published (8 each), then its Client Identifier as
 * a UTF-8 Encoded String.
 */
#define MQTT_STORE_RECORD_SESSION 0x01U

/** @brief Kind of record: a Session has ended. Its Client Identifier. */
#define MQTT_STORE_RECORD_SESSION_REMOVE 0x02U

/**
 * @brief Kind of record: a subscription, new or replaced. The Subscription
 * Options (1), flags (1), the Subscription Identifier (4), the Client
 * Identifier, then the Topic Filter, as it came in a SUBSCRIBE packet.
 */
#define MQTT_STORE_RECORD_SUBSCRIBE 0x03U

/**
 * @brief Kind of record: a subscription has gone. Flags (1), the Client
 * Identifier, then the Topic Filter.
 */
#define MQTT_STORE_RECORD_UNSUBSCRIBE 0x04U

/**
 * @brief Kind of record: a Session's Will Message. The store id of the
 * message (8), the Will Delay Interval (4), flags (1), the Message Expiry
 * Interval (4), then the Client Identifier.
 */
#define MQTT_STORE_RECORD_WILL 0x05U

/**
 * @brief Kind of record: a Will Message is gone, published or discarded. The
 * Client Identifier.
 */
#define MQTT_STORE_RECORD_WILL_REMOVE 0x06U

/**
 * @brief Kind of record: an Application Message. Its store id (8), QoS (1),
 * flags (1), the milliseconds left before it expires (8), the lengths of its
 * Topic Name (2), Properties (4) and Payload (4), then those bytes.
 */
#define MQTT_STORE_RECORD_MESSAGE 0x07U

/** @brief Kind of record: nothing holds a message any more. Its store id. */
#define MQTT_STORE_RECORD_MESSAGE_GONE 0x08U

/**
 * @brief Kind of record: a message's expiry has started. Its store id (8),
 * then the milliseconds left (8).
 */
#define MQTT_STORE_RECORD_MESSAGE_EXPIRY 0x09U

/**
 * @brief Kind of record: a message is the retained message of its Topic Name.
 * The message's store id.
 */
#define MQTT_STORE_RECORD_RETAINED 0x0AU

/**
 * @brief Kind of record: a Topic Name has no retained message any more. The
 * Topic Name.
 */
#define MQTT_STORE_RECORD_RETAINED_REMOVE 0x0BU

/**
 * @brief Kind of record: a message is waiting for a Session. The store id of
 * the delivery (8) and of the message (8), the QoS (1), flags (1), the
 * Subscription Identifier (4), then the Client Identifier.
 */
#define MQTT_STORE_RECORD_DELIVERY 0x0CU

/**
 * @brief Kind of record: a QoS 2 delivery has been sent. Its store id (8),
 * then the Packet Identifier it was sent with (2).
 */
#define MQTT_STORE_RECORD_SENT 0x0DU

/**
 * @brief Kind of record: a QoS 2 delivery has had its PUBREC, so its PUBREL is
 * what is owed. Its store id.
 */
#define MQTT_STORE_RECORD_RELEASED 0x0EU

/** @brief Kind of record: a delivery's flow is over. Its store id. */
#define MQTT_STORE_RECORD_FINISHED 0x0FU

/**
 * @brief Kind of record: a QoS 2 Packet Identifier received, waiting for its
 * PUBREL. The Packet Identifier (2), then the Client Identifier.
 */
#define MQTT_STORE_RECORD_INCOMING 0x10U

/**
 * @brief Kind of record: a QoS 2 Packet Identifier received has been released.
 * The Packet Identifier (2), then the Client Identifier.
 */
#define MQTT_STORE_RECORD_INCOMING_REMOVE 0x11U

/**
 * @brief Kind of client record: what the Client knows of its Session. The
 * protocol version (1), flags (1), then the Client Identifier as a UTF-8
 * Encoded String, which is empty if it isn't known.
 */
#define MQTT_STORE_RECORD_CLIENT_SESSION 0x21U

/**
 * @brief Kind of client record: a QoS 1 or QoS 2 message held for the Server.
 * Its Packet Identifier (2), stage (1), QoS (1), flags (1), Topic Alias (2),
 * then its PUBLISH packet as it goes out, which a message past PUBREC no
 * longer has. A journal holds only messages waiting to be sent: what becomes
 * of them is in the records after.
 */
#define MQTT_STORE_RECORD_CLIENT_MESSAGE 0x22U

/**
 * @brief Kind of client record: a message has been sent. Its Packet
 * Identifier.
 */
#define MQTT_STORE_RECORD_CLIENT_SENT 0x23U

/**
 * @brief Kind of client record: a QoS 2 message has had its PUBREC, so its
 * PUBREL is what is owed. Its Packet Identifier.
 */
#define MQTT_STORE_RECORD_CLIENT_RELEASED 0x24U

/**
 * @brief Kind of client record: a message is no longer held, as its flow is
 * over or it was given up. Its Packet Identifier.
 */
#define MQTT_STORE_RECORD_CLIENT_FINISHED 0x25U

/**
 * @brief Kind of client record: a QoS 2 Packet Identifier received, waiting
 * for its PUBREL. The Packet Identifier.
 */
#define MQTT_STORE_RECORD_CLIENT_INCOMING 0x26U

/**
 * @brief Kind of client record: a QoS 2 Packet Identifier received has been
 * released. The Packet Identifier.
 */
#define MQTT_STORE_RECORD_CLIENT_INCOMING_REMOVE 0x27U

/**
 * @brief Kind of client record: with MQTT 3.1, a SUBSCRIBE or UNSUBSCRIBE
 * packet waiting for its answer. Its Packet Identifier (2), then the packet,
 * which may be left out if it isn't kept to send again.
 */
#define MQTT_STORE_RECORD_CLIENT_PENDING 0x28U

/**
 * @brief Kind of client record: a SUBSCRIBE or UNSUBSCRIBE packet has had its
 * answer. Its Packet Identifier.
 */
#define MQTT_STORE_RECORD_CLIENT_PENDING_DONE 0x29U

/**
 * @brief Flag of a CLIENT_SESSION record: a Session exists that the Client
 * can resume. Without it, nothing sent or received is held any more.
 */
#define MQTT_STORE_CLIENT_EXISTS 0x01U

/** @brief Flag of a SESSION record: its Client is away. */
#define MQTT_STORE_SESSION_AWAY 0x01U

/** @brief Flag of a SESSION record: its Will Message waits out its delay. */
#define MQTT_STORE_SESSION_WILL_TIMER 0x02U

/** @brief Flag of SUBSCRIBE and UNSUBSCRIBE: a Shared Subscription. */
#define MQTT_STORE_SHARED 0x01U

/** @brief Flag of a WILL record: it has a Message Expiry Interval. */
#define MQTT_STORE_WILL_EXPIRY 0x01U

/** @brief Flag of MESSAGE and DELIVERY records: RETAIN is set. */
#define MQTT_STORE_RETAIN 0x01U

/** @brief Flag of a MESSAGE record: it has a Message Expiry Interval. */
#define MQTT_STORE_MESSAGE_EXPIRES 0x02U

/** @brief Number of content bytes in an END record. */
#define MQTT_STORE_END_SIZE 16U

/** @brief Number of content bytes in a COMMIT record. */
#define MQTT_STORE_COMMIT_SIZE 8U

/** @brief The most runs a record is made of. */
#define MQTT_STORE_RECORD_RUNS 6U

/** @brief What a record has to outlast before what depends on it goes out. */
typedef enum {
    /** @brief Nothing: it is not written at all. */
    MQTT_STORE_MEMORY = 0,
    /** @brief A crash of the process: written to the medium. */
    MQTT_STORE_WRITTEN,
    /** @brief A loss of power: written, and synced. */
    MQTT_STORE_SYNCED
} mqtt_store_durability;

/** @brief Which part of a store a place belongs to. */
typedef enum {
    /** @brief A copy of the snapshot. */
    MQTT_STORE_SNAPSHOT = 0,
    /** @brief A journal segment. */
    MQTT_STORE_JOURNAL
} mqtt_store_part;

/** @brief What was wrong with a store. */
typedef enum {
    /** @brief Nothing. */
    MQTT_STORE_FAULT_NONE = 0,
    /** @brief No snapshot copy passes its checks. */
    MQTT_STORE_FAULT_NO_COPY,
    /** @brief A segment header is damaged, or not one this library writes. */
    MQTT_STORE_FAULT_HEADER,
    /** @brief A segment has a newer major version of the format. */
    MQTT_STORE_FAULT_VERSION,
    /** @brief A record fails its check, and more bytes follow it. */
    MQTT_STORE_FAULT_RECORD,
    /** @brief A record passes its check but holds what may not be there. */
    MQTT_STORE_FAULT_CONTENT,
    /** @brief The segments don't follow on from the snapshot and each other. */
    MQTT_STORE_FAULT_CHAIN,
    /** @brief The storage given is too small for what is stored. */
    MQTT_STORE_FAULT_ROOM,
    /** @brief The medium could not be read. */
    MQTT_STORE_FAULT_MEDIUM,
    /**
     * @brief A record asks for what the configuration no longer offers, or
     * goes past one of its limits.
     */
    MQTT_STORE_FAULT_CONFIG
} mqtt_store_fault_kind;

/** @brief Where a store was found to be wrong, and how. */
typedef struct {
    /** @brief What was wrong. */
    mqtt_store_fault_kind kind;
    /** @brief The part of the store. */
    mqtt_store_part part;
    /** @brief The copy or the journal place. */
    uint32_t place;
    /** @brief The generation of the segment, or 0 if its header is unread. */
    uint64_t generation;
    /** @brief Offset of the record within its place, or 0 for the header. */
    uint64_t offset;
    /** @brief Kind of the record, or 0 for the header. */
    uint8_t record_kind;
} mqtt_store_fault;

/** @brief The fields of a segment header. */
typedef struct {
    /** @brief Counts the snapshots: the first is 1. */
    uint64_t generation;
    /**
     * @brief The last group the snapshot of this generation covers. A journal
     * segment's first group is the next.
     */
    uint64_t base;
    /** @brief Kind of segment, MQTT_STORE_SERVER_SNAPSHOT and the others. */
    uint8_t kind;
    /** @brief Major version of the format. */
    uint8_t major;
    /** @brief Minor version of the format. */
    uint8_t minor;
    /** @brief MQTT_STORE_FLAG_RECOVERY, or 0. */
    uint8_t flags;
} mqtt_store_header;

/** @brief A record to add to the end of the journal. */
typedef struct {
    /** @brief The record's bytes, frame and CRC included, in order. */
    const mqtt_run *runs;
    /** @brief Number of runs. */
    uint32_t run_count;
    /** @brief Number of bytes in all the runs. */
    uint32_t length;
    /** @brief The group the record belongs to. */
    uint64_t sequence;
    /**
     * @brief What the record has to outlast. For a COMMIT, the most any record
     * of its group has to: a sync is needed if that is MQTT_STORE_SYNCED.
     */
    mqtt_store_durability durability;
    /** @brief The kind of record. */
    uint8_t kind;
} mqtt_store_record;

/**
 * @brief Add a record to the end of the journal.
 *
 * The bytes are only valid during the call, so they are written or copied
 * before it returns. What depends on the record goes out once the application
 * says its group is stored.
 * @param[in] user_tag The value given with the function.
 * @param[in] record The record.
 * @return MQTT_OK, or any other value if it could not be added.
 */
typedef mqtt_status (*mqtt_store_append)(uint32_t user_tag,
                                         const mqtt_store_record *record);

/**
 * @brief Read bytes of a store's place.
 * @param[in] user_tag The value given with the function.
 * @param[in] part The part of the store.
 * @param[in] place The copy, or the journal place.
 * @param[in] offset Offset of the first byte to read.
 * @param[out] data Where the bytes go.
 * @param[in] capacity Number of bytes wanted.
 * @param[out] count Number of bytes read. It is less than capacity only at the
 * end of the place, and 0 past it or for a place that doesn't exist.
 * @return MQTT_OK, or any other value if the medium could not be read.
 */
typedef mqtt_status (*mqtt_store_read)(uint32_t user_tag, mqtt_store_part part,
                                       uint32_t place, uint64_t offset,
                                       uint8_t *data, uint32_t capacity,
                                       uint32_t *count);

/** @brief A store to read from. */
typedef struct {
    /** @brief The function that reads it. */
    mqtt_store_read read;
    /** @brief The value given to read. */
    uint32_t user_tag;
    /** @brief Number of snapshot copies. */
    uint32_t copies;
    /**
     * @brief The value of a byte never written: 0x00 in files, 0xFF in
     * flash.
     */
    uint8_t erased;
    /**
     * @brief Milliseconds from the last write to the store to the restore, or
     * 0 if the medium can't tell.
     */
    uint64_t downtime_ms;
} mqtt_store_source;

/** @brief What a scan of one place found. */
typedef struct {
    /** @brief The segment header. */
    mqtt_store_header header;
    /**
     * @brief Offset just past the last COMMIT of a journal, or past the END
     * of a snapshot: where what may be applied ends.
     */
    uint64_t commit_end;
    /** @brief Number of COMMIT records: the groups of a journal. */
    uint64_t commits;
    /** @brief Number of whole records, END left out. */
    uint64_t records;
    /** @brief The milliseconds of the last COMMIT, or 0 if there is none. */
    uint64_t elapsed_ms;
    /** @brief The next store id, from the END of a snapshot. */
    uint64_t next_id;
    /** @brief true if a record cut short at the end was left out. */
    bool torn;
} mqtt_store_scan;

/**
 * @brief Add bytes to a CRC-16 with polynomial 0x1021, not reflected and with
 * no final XOR. Started from MQTT_STORE_CRC_INIT, the nine bytes of the text
 * 123456789 give 0x29B1.
 * @param[in] crc The CRC of the bytes before these.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes in data.
 * @return The CRC with the bytes added, or crc as it was if data is NULL.
 */
uint16_t mqtt_store_crc16(uint16_t crc, const uint8_t *data, uint32_t length);

/**
 * @brief Write an 8-byte integer in network byte order.
 * @param[out] bytes Where the 8 bytes go.
 * @param[in] value The integer.
 */
void mqtt_store_put_u64(uint8_t *bytes, uint64_t value);

/**
 * @brief Read an 8-byte integer in network byte order.
 * @param[in] bytes The 8 bytes.
 * @return The integer, or 0 if bytes is NULL.
 */
uint64_t mqtt_store_get_u64(const uint8_t *bytes);

/**
 * @brief Write a segment header, with the version this library writes and its
 * CRC. The reserved bytes are zero.
 * @param[in] header The fields. Its major and minor are not used.
 * @param[out] bytes Where the MQTT_STORE_HEADER_SIZE bytes go.
 */
void mqtt_store_header_encode(const mqtt_store_header *header, uint8_t *bytes);

/**
 * @brief Read and check a segment header.
 * @param[in] bytes The MQTT_STORE_HEADER_SIZE bytes.
 * @param[in] kind The kind of segment expected.
 * @param[out] header The fields, when they pass.
 * @return MQTT_STORE_FAULT_NONE, MQTT_STORE_FAULT_VERSION for a major version
 * this library doesn't read, or MQTT_STORE_FAULT_HEADER for anything else
 * wrong: the magic number, the CRC, the kind, a flag, a reserved byte that
 * isn't zero in a minor version this library knows, or a NULL pointer.
 */
mqtt_store_fault_kind mqtt_store_header_decode(const uint8_t *bytes,
                                               uint8_t kind,
                                               mqtt_store_header *header);

/**
 * @brief Write the head that starts a record: its kind and length, and their
 * CRC.
 * @param[in] kind The kind of record.
 * @param[in] length Number of content bytes.
 * @param[out] bytes Where the MQTT_STORE_FRAME_HEAD bytes go.
 */
void mqtt_store_frame_head(uint8_t kind, uint32_t length, uint8_t *bytes);

/**
 * @brief Read the head that starts a record, and check its CRC.
 * @param[in] bytes The MQTT_STORE_FRAME_HEAD bytes.
 * @param[out] kind The kind of record.
 * @param[out] length Number of content bytes.
 * @return true if the head's CRC is right, so the length can be trusted. false,
 * with nothing written, if a pointer is NULL.
 */
bool mqtt_store_frame_check(const uint8_t *bytes, uint8_t *kind,
                            uint32_t *length);

/**
 * @brief Find what a record's CRC starts from before its content: the CRC of
 * its kind and length, which its head holds.
 *
 * The record's CRC carries that on over the content, and so leaves out the
 * head's own CRC. A CRC carried on over its own value gives zero, and stays
 * zero over zeros, so a record whose CRC took in its head's would check
 * whatever zeros followed a whole head, as a crash can leave in a file.
 *
 * @param[in] head The MQTT_STORE_FRAME_HEAD bytes.
 * @return The CRC, or 0 if head is NULL.
 */
uint16_t mqtt_store_frame_crc(const uint8_t *head);

/**
 * @brief Write the CRC that ends a record.
 * @param[in] crc The CRC-16 of the record's kind, length and content, from
 * mqtt_store_frame_crc on.
 * @param[out] bytes Where the MQTT_STORE_FRAME_TAIL bytes go.
 */
void mqtt_store_frame_tail(uint16_t crc, uint8_t *bytes);

/**
 * @brief Read one place of a store from start to end, and check its header
 * and the frame and CRC of each record. A record's content is checked only for
 * END and COMMIT.
 *
 * A snapshot is valid if it ends with END and nothing but erased bytes
 * follows. A journal is valid up to its last whole record, and a record cut
 * short at its end is left out.
 * @param[in] source The store.
 * @param[in] part The part of the store.
 * @param[in] place The copy, or the journal place.
 * @param[in] kind The kind of segment expected.
 * @param[out] buffer Room for the reads.
 * @param[in] size Number of bytes in buffer, at least MQTT_STORE_HEADER_SIZE.
 * @param[out] scan What was found, when the place is valid.
 * @param[out] fault What was wrong, when it isn't.
 * @return MQTT_OK if the place is valid; MQTT_NOT_FOUND if it holds nothing,
 * or only a header cut short; MQTT_STORE_DAMAGED with the fault otherwise; or
 * MQTT_INVALID_ARGUMENT if the buffer is too small, the source has no read
 * function, or a pointer is NULL. With scan or fault NULL, nothing is written.
 */
mqtt_status mqtt_store_scan_place(const mqtt_store_source *source,
                                  mqtt_store_part part, uint32_t place,
                                  uint8_t kind, uint8_t *buffer, uint32_t size,
                                  mqtt_store_scan *scan,
                                  mqtt_store_fault *fault);

/**
 * @brief Name a kind of fault, for a report.
 * @param[in] kind The kind of fault.
 * @return Words that describe it.
 */
const char *mqtt_store_fault_name(mqtt_store_fault_kind kind);

#endif /* MQTT_BASE_STORE_H */
