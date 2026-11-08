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
 * @file base_store.c
 * @brief The format of stored Session state: segments, records and checks.
 *
 * A scan reads a place through the read function it is given, a buffer at a
 * time, so a record of any size is checked in the room of one buffer. Only
 * the header and the content of END and COMMIT records are read as fields:
 * the rest is only added to the CRC.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 1.5, p.17-20. 4.1, p.92-93.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base_store.h"
#include "base_store_internal.h"
#include "base_utf8_t.h"

/** @brief Number of bytes in the magic number. */
#define STORE_MAGIC_SIZE 4U

/** @brief Offset of the major version in a header. */
#define STORE_AT_MAJOR 4U

/** @brief Offset of the minor version in a header. */
#define STORE_AT_MINOR 5U

/** @brief Offset of the kind of segment in a header. */
#define STORE_AT_KIND 6U

/** @brief Offset of the flags in a header. */
#define STORE_AT_FLAGS 7U

/** @brief Offset of the generation in a header. */
#define STORE_AT_GENERATION 8U

/** @brief Offset of the base sequence in a header. */
#define STORE_AT_BASE 16U

/** @brief Number of bytes of a record's head that its own CRC covers. */
#define STORE_FRAME_FIELDS 5U

/** @brief Offset of the reserved bytes in a header. */
#define STORE_AT_RESERVED 24U

/** @brief Offset of the CRC in a header, which covers every byte before it. */
#define STORE_AT_CRC 56U

/** @brief Number of entries in the table of the CRC. */
#define STORE_CRC_ENTRIES 256U

/** @brief A scan of one place in progress. */
typedef struct {
    /** @brief The store. */
    const mqtt_store_source *source;
    /** @brief The part of the store. */
    mqtt_store_part part;
    /** @brief The copy, or the journal place. */
    uint32_t place;
    /** @brief Room for the reads. */
    uint8_t *buffer;
    /** @brief Number of bytes in buffer. */
    uint32_t size;
    /** @brief What has been found so far. */
    mqtt_store_scan *scan;
    /** @brief Where a fault is described. */
    mqtt_store_fault *fault;
    /**
     * @brief Offset of the END record of a snapshot, once it has passed its
     * check, or 0.
     */
    uint64_t end_at;
    /** @brief Number of bytes of content of that END record. */
    uint32_t end_length;
} store_scanner;

/** @brief The bytes every segment starts with, L, T, E and R. */
static const uint8_t store_magic[STORE_MAGIC_SIZE] = {0x4CU, 0x54U, 0x45U,
                                                      0x52U};

/**
 * @brief Read a 2-byte integer in network byte order.
 * @param[in] bytes The 2 bytes.
 * @return The integer.
 */
static uint16_t store_get_u16(const uint8_t *bytes);

/**
 * @brief Read an integer of up to 4 bytes in network byte order.
 * @param[in] bytes The bytes.
 * @param[in] count Number of bytes, from 1 to 4.
 * @return The integer.
 */
static uint32_t store_get_u32(const uint8_t *bytes, uint32_t count);

/**
 * @brief Check the magic number and the CRC of a header.
 * @param[in] bytes The MQTT_STORE_HEADER_SIZE bytes.
 * @return true if both are right.
 */
static bool store_header_intact(const uint8_t *bytes);

/**
 * @brief Check that the reserved bytes of a header are zero, as a header of a
 * minor version this library knows must have them. A later minor version may
 * use them, and its fields are not read here.
 * @param[in] bytes The MQTT_STORE_HEADER_SIZE bytes.
 * @param[in] minor The header's minor version.
 * @return true if they are zero, or belong to a later minor version.
 */
static bool store_reserved_clear(const uint8_t *bytes, uint8_t minor);

/**
 * @brief Say whether a byte can start a record. 0x00 and 0xFF can't, which are
 * the bytes a file or a flash erase leaves.
 * @param[in] kind The byte.
 * @return true if it can.
 */
static bool store_kind_valid(uint8_t kind);

/**
 * @brief Describe a fault, and fail the scan.
 * @param[in,out] scanner The scan.
 * @param[in] kind What was wrong.
 * @param[in] offset Where, within the place.
 * @param[in] record_kind The kind of the record, or 0.
 * @return MQTT_STORE_DAMAGED.
 */
static mqtt_status store_damage(store_scanner *scanner,
                                mqtt_store_fault_kind kind, uint64_t offset,
                                uint8_t record_kind);

/**
 * @brief Read bytes of the place into the buffer.
 * @param[in,out] scanner The scan.
 * @param[in] offset Offset of the first byte.
 * @param[in] wanted Number of bytes wanted. No more than the buffer holds are
 * read.
 * @param[out] count Number of bytes read.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED if the medium failed.
 */
static mqtt_status store_read(store_scanner *scanner, uint64_t offset,
                              uint32_t wanted, uint32_t *count);

/**
 * @brief Find whether every byte from an offset to the end of the place is the
 * erased value.
 * @param[in,out] scanner The scan.
 * @param[in] offset Offset of the first byte.
 * @param[out] erased true if they all are.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED if the medium failed.
 */
static mqtt_status store_erased_from(store_scanner *scanner, uint64_t offset,
                                     bool *erased);

/**
 * @brief Add the content of a record to its CRC.
 * @param[in,out] scanner The scan.
 * @param[in] offset Offset of the first byte of the content.
 * @param[in] length Number of bytes of content.
 * @param[in,out] crc The CRC.
 * @param[out] whole false if the place ends first.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED if the medium failed.
 */
static mqtt_status store_crc_content(store_scanner *scanner, uint64_t offset,
                                     uint32_t length, uint16_t *crc,
                                     bool *whole);

/**
 * @brief Judge a record that fails its check, or the end of the place. If
 * every byte from an offset on is erased, the records end there, and a record
 * cut short was left out. Otherwise the place is damaged.
 * @param[in,out] scanner The scan.
 * @param[in] record Offset of the record.
 * @param[in] from Offset from which the bytes must be erased.
 * @param[in] kind The record's kind.
 * @param[in] cut true if a record was cut short there, false if the records
 * end cleanly when the bytes are erased.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status store_scan_tail(store_scanner *scanner, uint64_t record,
                                   uint64_t from, uint8_t kind, bool cut);

/**
 * @brief Check the content of a COMMIT record.
 * @param[in,out] scanner The scan.
 * @param[in] offset Offset of the record.
 * @param[in] length Number of bytes of content.
 * @param[in] after Offset just past the record.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status store_scan_commit(store_scanner *scanner, uint64_t offset,
                                     uint32_t length, uint64_t after);

/**
 * @brief Check the content of the END record of a snapshot, and that nothing
 * follows it. It is checked once the records have been read.
 * @param[in,out] scanner The scan.
 * @param[in] offset Offset of the record.
 * @param[in] length Number of bytes of content.
 * @param[in] after Offset just past the record.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status store_scan_end(store_scanner *scanner, uint64_t offset,
                                  uint32_t length, uint64_t after);

/**
 * @brief Count a record that passes its check, by its kind.
 * @param[in,out] scanner The scan.
 * @param[in] offset Offset of the record.
 * @param[in] kind The kind of the record.
 * @param[in] length Number of bytes of content.
 * @param[out] done true if the records end with this one.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status store_scan_kind(store_scanner *scanner, uint64_t offset,
                                   uint8_t kind, uint32_t length, bool *done);

/**
 * @brief Check a record whose kind and length have been read.
 * @param[in,out] scanner The scan.
 * @param[in] offset Offset of the record.
 * @param[out] next Offset of the record after it.
 * @param[out] done true if the records end here.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status store_scan_frame(store_scanner *scanner, uint64_t offset,
                                    uint64_t *next, bool *done);

/**
 * @brief Check the record at an offset.
 * @param[in,out] scanner The scan.
 * @param[in] offset Offset of the record.
 * @param[out] next Offset of the record after it.
 * @param[out] done true if the records end here.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status store_scan_record(store_scanner *scanner, uint64_t offset,
                                     uint64_t *next, bool *done);

/**
 * @brief Read and check the header of the place.
 * @param[in,out] scanner The scan.
 * @param[in] kind The kind of segment expected.
 * @return MQTT_OK, MQTT_NOT_FOUND or MQTT_STORE_DAMAGED.
 */
static mqtt_status store_scan_header(store_scanner *scanner, uint8_t kind);

/**
 * @brief Check every record of the place, after its header.
 * @param[in,out] scanner The scan.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status store_scan_records(store_scanner *scanner);

/**
 * @brief Note a fault found while choosing what to restore, unless one is
 * noted already. It has no place of its own.
 * @param[out] fault The fault.
 * @param[in] kind What was wrong.
 * @return MQTT_STORE_DAMAGED.
 */
static mqtt_status store_fail(mqtt_store_fault *fault,
                              mqtt_store_fault_kind kind);

/**
 * @brief Read and check every place of a store.
 * @param[in] source The store.
 * @param[in] snapshot_kind The kind of its snapshot segments.
 * @param[in] journal_kind The kind of its journal segments.
 * @param[out] buffer Room for the reads.
 * @param[in] size Number of bytes in buffer.
 * @param[out] copies What each snapshot copy holds.
 * @param[out] journals What each journal place holds.
 * @param[out] fault What was wrong with a journal place.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED if a journal place is damaged.
 */
static mqtt_status store_scan_all(const mqtt_store_source *source,
                                  uint8_t snapshot_kind, uint8_t journal_kind,
                                  uint8_t *buffer, uint32_t size,
                                  mqtt_store_place *copies,
                                  mqtt_store_place *journals,
                                  mqtt_store_fault *fault);

/**
 * @brief Choose the journal segments that follow a snapshot.
 * @param[in] journals What each journal place holds.
 * @param[in] generation The snapshot's generation.
 * @param[in] base The snapshot's base.
 * @param[out] chain The chain, whose journal places are set.
 * @param[out] fault What was wrong, if the segments don't follow on.
 * @return MQTT_OK if a chain follows on, MQTT_NOT_FOUND if none does, or
 * MQTT_STORE_DAMAGED if the segments don't follow on from each other.
 */
static mqtt_status store_follow(const mqtt_store_place *journals,
                                uint64_t generation, uint64_t base,
                                mqtt_store_chain *chain,
                                mqtt_store_fault *fault);

uint16_t mqtt_store_crc16(uint16_t crc, const uint8_t *data, uint32_t length)
{
    /**
     * @brief The CRC-16 of each value of a byte, shifted to the top, for the
     * polynomial 0x1021.
     */
    static const uint16_t table[STORE_CRC_ENTRIES] = {
        0x0000U, 0x1021U, 0x2042U, 0x3063U, 0x4084U, 0x50A5U, 0x60C6U, 0x70E7U,
        0x8108U, 0x9129U, 0xA14AU, 0xB16BU, 0xC18CU, 0xD1ADU, 0xE1CEU, 0xF1EFU,
        0x1231U, 0x0210U, 0x3273U, 0x2252U, 0x52B5U, 0x4294U, 0x72F7U, 0x62D6U,
        0x9339U, 0x8318U, 0xB37BU, 0xA35AU, 0xD3BDU, 0xC39CU, 0xF3FFU, 0xE3DEU,
        0x2462U, 0x3443U, 0x0420U, 0x1401U, 0x64E6U, 0x74C7U, 0x44A4U, 0x5485U,
        0xA56AU, 0xB54BU, 0x8528U, 0x9509U, 0xE5EEU, 0xF5CFU, 0xC5ACU, 0xD58DU,
        0x3653U, 0x2672U, 0x1611U, 0x0630U, 0x76D7U, 0x66F6U, 0x5695U, 0x46B4U,
        0xB75BU, 0xA77AU, 0x9719U, 0x8738U, 0xF7DFU, 0xE7FEU, 0xD79DU, 0xC7BCU,
        0x48C4U, 0x58E5U, 0x6886U, 0x78A7U, 0x0840U, 0x1861U, 0x2802U, 0x3823U,
        0xC9CCU, 0xD9EDU, 0xE98EU, 0xF9AFU, 0x8948U, 0x9969U, 0xA90AU, 0xB92BU,
        0x5AF5U, 0x4AD4U, 0x7AB7U, 0x6A96U, 0x1A71U, 0x0A50U, 0x3A33U, 0x2A12U,
        0xDBFDU, 0xCBDCU, 0xFBBFU, 0xEB9EU, 0x9B79U, 0x8B58U, 0xBB3BU, 0xAB1AU,
        0x6CA6U, 0x7C87U, 0x4CE4U, 0x5CC5U, 0x2C22U, 0x3C03U, 0x0C60U, 0x1C41U,
        0xEDAEU, 0xFD8FU, 0xCDECU, 0xDDCDU, 0xAD2AU, 0xBD0BU, 0x8D68U, 0x9D49U,
        0x7E97U, 0x6EB6U, 0x5ED5U, 0x4EF4U, 0x3E13U, 0x2E32U, 0x1E51U, 0x0E70U,
        0xFF9FU, 0xEFBEU, 0xDFDDU, 0xCFFCU, 0xBF1BU, 0xAF3AU, 0x9F59U, 0x8F78U,
        0x9188U, 0x81A9U, 0xB1CAU, 0xA1EBU, 0xD10CU, 0xC12DU, 0xF14EU, 0xE16FU,
        0x1080U, 0x00A1U, 0x30C2U, 0x20E3U, 0x5004U, 0x4025U, 0x7046U, 0x6067U,
        0x83B9U, 0x9398U, 0xA3FBU, 0xB3DAU, 0xC33DU, 0xD31CU, 0xE37FU, 0xF35EU,
        0x02B1U, 0x1290U, 0x22F3U, 0x32D2U, 0x4235U, 0x5214U, 0x6277U, 0x7256U,
        0xB5EAU, 0xA5CBU, 0x95A8U, 0x8589U, 0xF56EU, 0xE54FU, 0xD52CU, 0xC50DU,
        0x34E2U, 0x24C3U, 0x14A0U, 0x0481U, 0x7466U, 0x6447U, 0x5424U, 0x4405U,
        0xA7DBU, 0xB7FAU, 0x8799U, 0x97B8U, 0xE75FU, 0xF77EU, 0xC71DU, 0xD73CU,
        0x26D3U, 0x36F2U, 0x0691U, 0x16B0U, 0x6657U, 0x7676U, 0x4615U, 0x5634U,
        0xD94CU, 0xC96DU, 0xF90EU, 0xE92FU, 0x99C8U, 0x89E9U, 0xB98AU, 0xA9ABU,
        0x5844U, 0x4865U, 0x7806U, 0x6827U, 0x18C0U, 0x08E1U, 0x3882U, 0x28A3U,
        0xCB7DU, 0xDB5CU, 0xEB3FU, 0xFB1EU, 0x8BF9U, 0x9BD8U, 0xABBBU, 0xBB9AU,
        0x4A75U, 0x5A54U, 0x6A37U, 0x7A16U, 0x0AF1U, 0x1AD0U, 0x2AB3U, 0x3A92U,
        0xFD2EU, 0xED0FU, 0xDD6CU, 0xCD4DU, 0xBDAAU, 0xAD8BU, 0x9DE8U, 0x8DC9U,
        0x7C26U, 0x6C07U, 0x5C64U, 0x4C45U, 0x3CA2U, 0x2C83U, 0x1CE0U, 0x0CC1U,
        0xEF1FU, 0xFF3EU, 0xCF5DU, 0xDF7CU, 0xAF9BU, 0xBFBAU, 0x8FD9U, 0x9FF8U,
        0x6E17U, 0x7E36U, 0x4E55U, 0x5E74U, 0x2E93U, 0x3EB2U, 0x0ED1U, 0x1EF0U
    };
    /** @brief The CRC so far. */
    uint32_t value = (uint32_t)crc;
    /** @brief Index of the next byte. */
    uint32_t index = 0U;
    /** @brief Number of bytes to take: none if there are no bytes. */
    const uint32_t end = (data != NULL) ? length : 0U;

    while (index < end) {
        /** @brief The entry of the table this byte selects. */
        const uint32_t entry = ((value >> 8U) ^ (uint32_t)data[index]) & 0xFFU;

        value = ((value << 8U) & 0xFFFFU) ^ (uint32_t)table[entry];
        index += 1U;
    }
    return (uint16_t)value;
}

/**
 * @brief Do what mqtt_store_put_u64 does, for arguments known to be valid.
 * @param[out] bytes Where the 8 bytes go.
 * @param[in] value The integer.
 */
static void store_put_u64_body(uint8_t *bytes, uint64_t value)
{
    /** @brief The bits not written yet. */
    uint64_t rest = value;
    /** @brief Index of the byte being written, from the last. */
    uint32_t index = 8U;

    while (index > 0U) {
        index -= 1U;
        bytes[index] = (uint8_t)(rest & 0xFFU);
        rest >>= 8U;
    }
}

void mqtt_store_put_u64(uint8_t *bytes, uint64_t value)
{
    if (bytes != NULL) {
        store_put_u64_body(bytes, value);
    }
}

/**
 * @brief Do what mqtt_store_get_u64 does, for arguments known to be valid.
 * @param[in] bytes The 8 bytes.
 * @return The integer.
 */
static uint64_t store_get_u64_body(const uint8_t *bytes)
{
    /** @brief The integer so far. */
    uint64_t value = 0U;
    /** @brief Index of the byte being read. */
    uint32_t index = 0U;

    while (index < 8U) {
        value = (value << 8U) | (uint64_t)bytes[index];
        index += 1U;
    }
    return value;
}

uint64_t mqtt_store_get_u64(const uint8_t *bytes)
{
    /** @brief The answer. */
    uint64_t answer = 0U;

    if (bytes != NULL) {
        answer = store_get_u64_body(bytes);
    }
    return answer;
}

/**
 * @brief Do what mqtt_store_header_encode does, for arguments known to be
 * valid.
 * @param[in] header The fields. Its major and minor are not used.
 * @param[out] bytes Where the MQTT_STORE_HEADER_SIZE bytes go.
 */
static void store_header_encode_body(const mqtt_store_header *header,
                                     uint8_t *bytes)
{
    /** @brief Index of the byte being written. */
    uint32_t index = 0U;
    /** @brief The CRC of the bytes before it. */
    uint16_t crc = 0U;

    while (index < STORE_MAGIC_SIZE) {
        bytes[index] = store_magic[index];
        index += 1U;
    }
    bytes[STORE_AT_MAJOR] = (uint8_t)MQTT_STORE_FORMAT_MAJOR;
    bytes[STORE_AT_MINOR] = (uint8_t)MQTT_STORE_FORMAT_MINOR;
    bytes[STORE_AT_KIND] = header->kind;
    bytes[STORE_AT_FLAGS] = header->flags;
    mqtt_store_put_u64(&bytes[STORE_AT_GENERATION], header->generation);
    mqtt_store_put_u64(&bytes[STORE_AT_BASE], header->base);
    index = STORE_AT_RESERVED;
    while (index < STORE_AT_CRC) {
        bytes[index] = 0U;
        index += 1U;
    }
    crc = mqtt_store_crc16((uint16_t)MQTT_STORE_CRC_INIT, bytes, STORE_AT_CRC);
    mqtt_store_frame_tail(crc, &bytes[STORE_AT_CRC]);
}

void mqtt_store_header_encode(const mqtt_store_header *header, uint8_t *bytes)
{
    if ((header != NULL) && (bytes != NULL)) {
        store_header_encode_body(header, bytes);
    }
}

/**
 * @brief Do what mqtt_store_header_decode does, for arguments known to be
 * valid.
 * @param[in] bytes The MQTT_STORE_HEADER_SIZE bytes.
 * @param[in] kind The kind of segment expected.
 * @param[out] header The fields, when they pass.
 * @return MQTT_STORE_FAULT_NONE, MQTT_STORE_FAULT_VERSION for a major version
 * this library doesn't read, or MQTT_STORE_FAULT_HEADER for anything else
 * wrong: the magic number, the CRC, the kind, a flag, or a reserved byte that
 * isn't zero in a minor version this library knows.
 */
static mqtt_store_fault_kind store_header_decode_body(const uint8_t *bytes,
                                                      uint8_t kind,
                                                      mqtt_store_header *header)
{
    /** @brief What was wrong. */
    mqtt_store_fault_kind fault = MQTT_STORE_FAULT_HEADER;

    if (store_header_intact(bytes)) {
        /** @brief The flags a segment of this kind may have. */
        const uint32_t allowed = ((kind == MQTT_STORE_SERVER_JOURNAL) ||
                                  (kind == MQTT_STORE_CLIENT_JOURNAL)) ?
                                     MQTT_STORE_FLAG_RECOVERY :
                                     0U;
        /** @brief The flags it has. */
        const uint32_t flags = (uint32_t)bytes[STORE_AT_FLAGS];

        if ((uint32_t)bytes[STORE_AT_MAJOR] > MQTT_STORE_FORMAT_MAJOR) {
            fault = MQTT_STORE_FAULT_VERSION;
        } else if (((uint32_t)bytes[STORE_AT_MAJOR] ==
                    MQTT_STORE_FORMAT_MAJOR) &&
                   (bytes[STORE_AT_KIND] == kind) &&
                   ((flags & ~allowed) == 0U) &&
                   store_reserved_clear(bytes, bytes[STORE_AT_MINOR])) {
            fault = MQTT_STORE_FAULT_NONE;
            header->generation =
                mqtt_store_get_u64(&bytes[STORE_AT_GENERATION]);
            header->base = mqtt_store_get_u64(&bytes[STORE_AT_BASE]);
            header->kind = kind;
            header->major = bytes[STORE_AT_MAJOR];
            header->minor = bytes[STORE_AT_MINOR];
            header->flags = bytes[STORE_AT_FLAGS];
        } else {
            /* A major version of 0, another kind, a flag or a reserved
             * byte: the fault stays MQTT_STORE_FAULT_HEADER. */
        }
    }
    return fault;
}

mqtt_store_fault_kind mqtt_store_header_decode(const uint8_t *bytes,
                                               uint8_t kind,
                                               mqtt_store_header *header)
{
    /** @brief The answer. */
    mqtt_store_fault_kind answer = MQTT_STORE_FAULT_HEADER;

    if ((bytes != NULL) && (header != NULL)) {
        answer = store_header_decode_body(bytes, kind, header);
    }
    return answer;
}

/**
 * @brief Do what mqtt_store_frame_head does, for arguments known to be valid.
 * @param[in] kind The kind of record.
 * @param[in] length Number of content bytes.
 * @param[out] bytes Where the MQTT_STORE_FRAME_HEAD bytes go.
 */
static void store_frame_head_body(uint8_t kind, uint32_t length, uint8_t *bytes)
{
    /** @brief The CRC of the kind and the length. */
    uint16_t crc = 0U;

    bytes[0] = kind;
    bytes[1] = (uint8_t)((length >> 24U) & 0xFFU);
    bytes[2] = (uint8_t)((length >> 16U) & 0xFFU);
    bytes[3] = (uint8_t)((length >> 8U) & 0xFFU);
    bytes[4] = (uint8_t)(length & 0xFFU);
    crc = mqtt_store_frame_crc(bytes);
    mqtt_store_frame_tail(crc, &bytes[STORE_FRAME_FIELDS]);
}

void mqtt_store_frame_head(uint8_t kind, uint32_t length, uint8_t *bytes)
{
    if (bytes != NULL) {
        store_frame_head_body(kind, length, bytes);
    }
}

/**
 * @brief Do what mqtt_store_frame_crc does, for arguments known to be valid.
 * @param[in] head The MQTT_STORE_FRAME_HEAD bytes.
 * @return The CRC.
 */
static uint16_t store_frame_crc_body(const uint8_t *head)
{
    return mqtt_store_crc16((uint16_t)MQTT_STORE_CRC_INIT, head,
                            STORE_FRAME_FIELDS);
}

uint16_t mqtt_store_frame_crc(const uint8_t *head)
{
    /** @brief The answer. */
    uint16_t answer = 0U;

    if (head != NULL) {
        answer = store_frame_crc_body(head);
    }
    return answer;
}

/**
 * @brief Do what mqtt_store_frame_check does, for arguments known to be valid.
 * @param[in] bytes The MQTT_STORE_FRAME_HEAD bytes.
 * @param[out] kind The kind of record.
 * @param[out] length Number of content bytes.
 * @return true if the head's CRC is right, so the length can be trusted.
 */
static bool store_frame_check_body(const uint8_t *bytes, uint8_t *kind,
                                   uint32_t *length)
{
    *kind = bytes[0];
    *length = store_get_u32(&bytes[1], 4U);
    return store_get_u16(&bytes[STORE_FRAME_FIELDS]) ==
           mqtt_store_frame_crc(bytes);
}

bool mqtt_store_frame_check(const uint8_t *bytes, uint8_t *kind,
                            uint32_t *length)
{
    /** @brief The answer. */
    bool answer = false;

    if ((bytes != NULL) && (kind != NULL) && (length != NULL)) {
        answer = store_frame_check_body(bytes, kind, length);
    }
    return answer;
}

void mqtt_store_frame_tail(uint16_t crc, uint8_t *bytes)
{
    if (bytes != NULL) {
        bytes[0] = (uint8_t)(((uint32_t)crc >> 8U) & 0xFFU);
        bytes[1] = (uint8_t)((uint32_t)crc & 0xFFU);
    }
}

/**
 * @brief Do what mqtt_store_scan_place does, for arguments known to be valid.
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
 * MQTT_INVALID_ARGUMENT if the buffer is too small or the source has no read
 * function.
 */
static mqtt_status store_scan_place_body(const mqtt_store_source *source,
                                         mqtt_store_part part, uint32_t place,
                                         uint8_t kind, uint8_t *buffer,
                                         uint32_t size, mqtt_store_scan *scan,
                                         mqtt_store_fault *fault)
{
    /** @brief The scan. */
    store_scanner scanner = {source, part, place, buffer, size, scan, fault,
                             0U, 0U};
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_ARGUMENT;

    scan->header.generation = 0U;
    scan->header.base = 0U;
    scan->header.kind = 0U;
    scan->header.major = 0U;
    scan->header.minor = 0U;
    scan->header.flags = 0U;
    scan->commit_end = MQTT_STORE_HEADER_SIZE;
    scan->commits = 0U;
    scan->records = 0U;
    scan->elapsed_ms = 0U;
    scan->next_id = 0U;
    scan->torn = false;
    fault->kind = MQTT_STORE_FAULT_NONE;
    fault->part = part;
    fault->place = place;
    fault->generation = 0U;
    fault->offset = 0U;
    fault->record_kind = 0U;
    if ((source != NULL) && (source->read != NULL) && (buffer != NULL) &&
        (size >= MQTT_STORE_HEADER_SIZE)) {
        status = store_scan_header(&scanner, kind);
        if (status == MQTT_OK) {
            status = store_scan_records(&scanner);
        }
    }
    return status;
}

mqtt_status mqtt_store_scan_place(const mqtt_store_source *source,
                                  mqtt_store_part part, uint32_t place,
                                  uint8_t kind, uint8_t *buffer, uint32_t size,
                                  mqtt_store_scan *scan,
                                  mqtt_store_fault *fault)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if ((scan != NULL) && (fault != NULL)) {
        answer = store_scan_place_body(source, part, place, kind, buffer, size,
                                       scan, fault);
    }
    return answer;
}

const char *mqtt_store_fault_name(mqtt_store_fault_kind kind)
{
    /** @brief The words. */
    const char *name = "no fault";

    switch (kind) {
    case MQTT_STORE_FAULT_NO_COPY:
        name = "no snapshot copy passes its checks";
        break;
    case MQTT_STORE_FAULT_HEADER:
        name = "a segment header is damaged or of another kind";
        break;
    case MQTT_STORE_FAULT_VERSION:
        name = "a segment has a newer major version of the format";
        break;
    case MQTT_STORE_FAULT_RECORD:
        name = "a record fails its check and more bytes follow it";
        break;
    case MQTT_STORE_FAULT_CONTENT:
        name = "a record holds what may not be there";
        break;
    case MQTT_STORE_FAULT_CHAIN:
        name = "the segments don't follow on from the snapshot";
        break;
    case MQTT_STORE_FAULT_ROOM:
        name = "the storage given is too small for what is stored";
        break;
    case MQTT_STORE_FAULT_MEDIUM:
        name = "the medium could not be read";
        break;
    case MQTT_STORE_FAULT_CONFIG:
        name = "a record asks for what the configuration doesn't offer";
        break;
    default:
        /* MQTT_STORE_FAULT_NONE keeps the name it was given. */
        break;
    }
    return name;
}

void mqtt_journal_start(mqtt_journal_builder *builder, uint8_t *bytes,
                        uint32_t capacity, mqtt_run *runs)
{
    builder->bytes = bytes;
    builder->capacity = capacity;
    builder->runs = runs;
    builder->used = 0U;
    builder->count = 0U;
    builder->length = 0U;
    builder->in_scratch = false;
    builder->failed = false;
    (void)mqtt_journal_room(builder, MQTT_STORE_FRAME_HEAD);
}

uint8_t *mqtt_journal_room(mqtt_journal_builder *builder, uint32_t count)
{
    /** @brief Where the bytes go. */
    uint8_t *room = builder->sink;

    if ((count > (builder->capacity - builder->used)) ||
        ((!builder->in_scratch) &&
         (builder->count >= MQTT_STORE_RECORD_RUNS))) {
        builder->failed = true;
    } else {
        if (!builder->in_scratch) {
            builder->runs[builder->count].data = &builder->bytes[builder->used];
            builder->runs[builder->count].length = 0U;
            builder->count += 1U;
            builder->in_scratch = true;
        }
        room = &builder->bytes[builder->used];
        builder->used += count;
        builder->runs[builder->count - 1U].length += count;
        builder->length += count;
    }
    return room;
}

void mqtt_journal_integer(mqtt_journal_builder *builder, uint64_t value,
                          uint32_t count)
{
    /** @brief The number of bytes written, no more than the sink holds. */
    const uint32_t size = (count <= MQTT_JOURNAL_SINK) ? count :
                                                         MQTT_JOURNAL_SINK;
    /** @brief Where the bytes go. */
    uint8_t *room = mqtt_journal_room(builder, size);
    /** @brief The bits not written yet. */
    uint64_t rest = value;
    /** @brief Index of the byte being written, from the last. */
    uint32_t index = size;

    while (index > 0U) {
        index -= 1U;
        room[index] = (uint8_t)(rest & 0xFFU);
        rest >>= 8U;
    }
}

void mqtt_journal_span(mqtt_journal_builder *builder, const uint8_t *data,
                       uint32_t length)
{
    if (length == 0U) {
        /* Nothing to add. */
    } else if ((builder->count >= MQTT_STORE_RECORD_RUNS) ||
               (length > (UINT32_MAX - builder->length))) {
        builder->failed = true;
    } else {
        builder->runs[builder->count].data = data;
        builder->runs[builder->count].length = length;
        builder->count += 1U;
        builder->length += length;
        builder->in_scratch = false;
    }
}

void mqtt_journal_string(mqtt_journal_builder *builder, const mqtt_utf8 *text)
{
    mqtt_journal_integer(builder, (uint64_t)text->length, 2U);
    mqtt_journal_span(builder, text->data, (uint32_t)text->length);
}

bool mqtt_journal_finish(mqtt_journal_builder *builder, uint8_t kind)
{
    if ((builder->length - MQTT_STORE_FRAME_HEAD) > MQTT_STORE_CONTENT_MAX) {
        builder->failed = true;
    }
    if (!builder->failed) {
        /** @brief The CRC of the record. */
        uint16_t crc = 0U;
        /** @brief Index of the run being added to the CRC. */
        uint32_t index = 1U;

        mqtt_store_frame_head(kind, builder->length - MQTT_STORE_FRAME_HEAD,
                              builder->bytes);
        /* The first run starts with the head, whose own CRC is left out. */
        crc = mqtt_store_crc16(mqtt_store_frame_crc(builder->bytes),
                               &builder->runs[0].data[MQTT_STORE_FRAME_HEAD],
                               builder->runs[0].length -
                                   MQTT_STORE_FRAME_HEAD);
        while (index < builder->count) {
            crc = mqtt_store_crc16(crc, builder->runs[index].data,
                                   builder->runs[index].length);
            index += 1U;
        }
        mqtt_store_frame_tail(crc, mqtt_journal_room(builder,
                                                     MQTT_STORE_FRAME_TAIL));
    }
    return !builder->failed;
}

mqtt_status mqtt_store_choose(const mqtt_store_source *source,
                              uint8_t snapshot_kind, uint8_t journal_kind,
                              uint8_t *buffer, uint32_t size,
                              mqtt_store_place *copies,
                              mqtt_store_place *journals,
                              mqtt_store_chain *chain,
                              mqtt_store_fault *fault)
{
    /** @brief The newest generation tried so far, from above. */
    uint64_t below = UINT64_MAX;
    /** @brief true if a copy holds anything at all. */
    bool written = false;
    /** @brief The outcome. */
    mqtt_status status =
        store_scan_all(source, snapshot_kind, journal_kind, buffer, size,
                       copies, journals, fault);

    chain->copy = MQTT_STORE_PLACE_NONE;
    chain->first = MQTT_STORE_PLACE_NONE;
    chain->second = MQTT_STORE_PLACE_NONE;
    if (status == MQTT_OK) {
        status = MQTT_NOT_FOUND;
    }
    while (status == MQTT_NOT_FOUND) {
        /** @brief The newest valid copy below the last tried. */
        uint32_t best = MQTT_STORE_PLACE_NONE;
        /** @brief Index of the copy. */
        uint32_t copy = 0U;

        while (copy < source->copies) {
            if (copies[copy].status != MQTT_NOT_FOUND) {
                written = true;
            }
            if ((copies[copy].status == MQTT_OK) &&
                (copies[copy].scan.header.generation < below) &&
                ((best == MQTT_STORE_PLACE_NONE) ||
                 (copies[copy].scan.header.generation >
                  copies[best].scan.header.generation))) {
                best = copy;
            }
            copy += 1U;
        }
        if (best == MQTT_STORE_PLACE_NONE) {
            break;
        }
        below = copies[best].scan.header.generation;
        status = store_follow(journals, below, copies[best].scan.header.base,
                              chain, fault);
        chain->copy = best;
    }
    if (status == MQTT_NOT_FOUND) {
        /* No snapshot is whole. The first generation's starts empty, so its
         * segment is enough. */
        chain->copy = MQTT_STORE_PLACE_NONE;
        status = store_follow(journals, 1U, 0U, chain, fault);
        if ((status == MQTT_NOT_FOUND) &&
            (written || (journals[0].status == MQTT_OK) ||
             (journals[1].status == MQTT_OK))) {
            status = store_fail(fault, MQTT_STORE_FAULT_NO_COPY);
        }
    }
    return status;
}

static uint16_t store_get_u16(const uint8_t *bytes)
{
    return (uint16_t)store_get_u32(bytes, 2U);
}

static uint32_t store_get_u32(const uint8_t *bytes, uint32_t count)
{
    /** @brief The integer so far. */
    uint32_t value = 0U;
    /** @brief Index of the byte being read. */
    uint32_t index = 0U;

    /* A byte at a time: some compilers turn the four shifts written out into
     * a call to a function of their own library to swap the bytes. */
    while (index < count) {
        value = (value << 8U) | (uint32_t)bytes[index];
        index += 1U;
    }
    return value;
}

static bool store_header_intact(const uint8_t *bytes)
{
    /** @brief true while every byte checked is right. */
    bool intact = true;
    /** @brief Index of the byte of the magic number being checked. */
    uint32_t index = 0U;

    while (index < STORE_MAGIC_SIZE) {
        if (bytes[index] != store_magic[index]) {
            intact = false;
        }
        index += 1U;
    }
    if (intact) {
        intact = (mqtt_store_crc16((uint16_t)MQTT_STORE_CRC_INIT, bytes,
                                   STORE_AT_CRC) ==
                  store_get_u16(&bytes[STORE_AT_CRC]));
    }
    return intact;
}

static bool store_reserved_clear(const uint8_t *bytes, uint8_t minor)
{
    /** @brief true while every byte checked is zero. */
    bool clear = true;

    if ((uint32_t)minor > MQTT_STORE_FORMAT_MINOR) {
        /* The bytes may hold fields this library doesn't know. */
    } else {
        /** @brief Index of the byte being checked. */
        uint32_t index = STORE_AT_RESERVED;

        while (index < STORE_AT_CRC) {
            if (bytes[index] != 0U) {
                clear = false;
            }
            index += 1U;
        }
    }
    return clear;
}

static bool store_kind_valid(uint8_t kind)
{
    return (kind != 0x00U) && (kind != 0xFFU);
}

static mqtt_status store_damage(store_scanner *scanner,
                                mqtt_store_fault_kind kind, uint64_t offset,
                                uint8_t record_kind)
{
    if (scanner->fault->kind == MQTT_STORE_FAULT_NONE) {
        scanner->fault->kind = kind;
        scanner->fault->generation = scanner->scan->header.generation;
        scanner->fault->offset = offset;
        scanner->fault->record_kind = record_kind;
    }
    return MQTT_STORE_DAMAGED;
}

static mqtt_status store_read(store_scanner *scanner, uint64_t offset,
                              uint32_t wanted, uint32_t *count)
{
    /** @brief Number of bytes asked for. */
    const uint32_t capacity = (wanted < scanner->size) ? wanted : scanner->size;
    /** @brief Number of bytes read. */
    uint32_t got = 0U;
    /** @brief The outcome. */
    mqtt_status status = scanner->source->read(
        scanner->source->user_tag, scanner->part, scanner->place, offset,
        scanner->buffer, capacity, &got);

    if ((status != MQTT_OK) || (got > capacity)) {
        status = store_damage(scanner, MQTT_STORE_FAULT_MEDIUM, offset, 0U);
        got = 0U;
    }
    *count = got;
    return status;
}

static mqtt_status store_erased_from(store_scanner *scanner, uint64_t offset,
                                     bool *erased)
{
    /** @brief Offset of the next read. */
    uint64_t at = offset;
    /** @brief true while every byte read is erased. */
    bool clear = true;
    /** @brief true while the place may hold more bytes. */
    bool more = true;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    while (more && clear && (status == MQTT_OK)) {
        /** @brief Number of bytes read. */
        uint32_t count = 0U;
        /** @brief Index of the byte being checked. */
        uint32_t index = 0U;

        status = store_read(scanner, at, scanner->size, &count);
        while (index < count) {
            if (scanner->buffer[index] != scanner->source->erased) {
                clear = false;
            }
            index += 1U;
        }
        more = (count == scanner->size);
        at += (uint64_t)count;
    }
    *erased = clear;
    return status;
}

static mqtt_status store_crc_content(store_scanner *scanner, uint64_t offset,
                                     uint32_t length, uint16_t *crc,
                                     bool *whole)
{
    /** @brief Offset of the next read. */
    uint64_t at = offset;
    /** @brief Number of bytes still to add. */
    uint32_t left = length;
    /** @brief true while each read gives all it was asked for. */
    bool full = true;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    while ((left > 0U) && full && (status == MQTT_OK)) {
        /** @brief Number of bytes asked for. */
        const uint32_t wanted = (left < scanner->size) ? left : scanner->size;
        /** @brief Number of bytes read. */
        uint32_t count = 0U;

        status = store_read(scanner, at, wanted, &count);
        *crc = mqtt_store_crc16(*crc, scanner->buffer, count);
        full = (count == wanted);
        left -= count;
        at += (uint64_t)count;
    }
    *whole = (left == 0U);
    return status;
}

static mqtt_status store_scan_tail(store_scanner *scanner, uint64_t record,
                                   uint64_t from, uint8_t kind, bool cut)
{
    /** @brief true if every byte from the offset on is erased. */
    bool erased = false;
    /** @brief The outcome. */
    mqtt_status status = store_erased_from(scanner, from, &erased);

    if ((status == MQTT_OK) && (!erased)) {
        status = store_damage(scanner, MQTT_STORE_FAULT_RECORD, record, kind);
    } else if ((status == MQTT_OK) && cut) {
        scanner->scan->torn = true;
    } else {
        /* The records end cleanly, or the medium failed. */
    }
    return status;
}

static mqtt_status store_scan_commit(store_scanner *scanner, uint64_t offset,
                                     uint32_t length, uint64_t after)
{
    /** @brief Number of bytes read. */
    uint32_t count = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (length != MQTT_STORE_COMMIT_SIZE) {
        status = store_damage(scanner, MQTT_STORE_FAULT_CONTENT, offset,
                              (uint8_t)MQTT_STORE_RECORD_COMMIT);
    } else {
        status = store_read(scanner, offset + MQTT_STORE_FRAME_HEAD, length,
                            &count);
    }
    if ((status == MQTT_OK) && (count == MQTT_STORE_COMMIT_SIZE)) {
        /** @brief The milliseconds the record gives. */
        const uint64_t elapsed = mqtt_store_get_u64(scanner->buffer);

        if (elapsed < scanner->scan->elapsed_ms) {
            status = store_damage(scanner, MQTT_STORE_FAULT_CONTENT, offset,
                                  (uint8_t)MQTT_STORE_RECORD_COMMIT);
        } else {
            scanner->scan->elapsed_ms = elapsed;
            scanner->scan->commit_end = after;
            scanner->scan->commits += 1U;
        }
    } else if (status == MQTT_OK) {
        /* The place changed between the two reads. */
        status = store_damage(scanner, MQTT_STORE_FAULT_MEDIUM, offset, 0U);
    } else {
        /* The fault is already described. */
    }
    return status;
}

static mqtt_status store_scan_end(store_scanner *scanner, uint64_t offset,
                                  uint32_t length, uint64_t after)
{
    /** @brief Number of bytes read. */
    uint32_t count = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (length != MQTT_STORE_END_SIZE) {
        status = store_damage(scanner, MQTT_STORE_FAULT_CONTENT, offset,
                              (uint8_t)MQTT_STORE_RECORD_END);
    } else {
        status = store_read(scanner, offset + MQTT_STORE_FRAME_HEAD, length,
                            &count);
    }
    if ((status == MQTT_OK) && (count == MQTT_STORE_END_SIZE)) {
        /** @brief true if every byte after the record is erased. */
        bool erased = false;

        if (mqtt_store_get_u64(scanner->buffer) != scanner->scan->records) {
            status = store_damage(scanner, MQTT_STORE_FAULT_CONTENT, offset,
                                  (uint8_t)MQTT_STORE_RECORD_END);
        } else {
            scanner->scan->next_id = mqtt_store_get_u64(&scanner->buffer[8]);
            status = store_erased_from(scanner, after, &erased);
        }
        if ((status == MQTT_OK) && (!erased)) {
            status = store_damage(scanner, MQTT_STORE_FAULT_RECORD, after, 0U);
        } else if (status == MQTT_OK) {
            scanner->scan->commit_end = after;
        } else {
            /* The fault is already described. */
        }
    } else if (status == MQTT_OK) {
        status = store_damage(scanner, MQTT_STORE_FAULT_MEDIUM, offset, 0U);
    } else {
        /* The fault is already described. */
    }
    return status;
}

static mqtt_status store_scan_kind(store_scanner *scanner, uint64_t offset,
                                   uint8_t kind, uint32_t length, bool *done)
{
    /** @brief Offset just past the record. */
    const uint64_t after = offset + MQTT_STORE_FRAME_HEAD + (uint64_t)length +
                           MQTT_STORE_FRAME_TAIL;
    /** @brief true for a snapshot copy, false for a journal segment. */
    const bool snapshot = (scanner->part == MQTT_STORE_SNAPSHOT);
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    *done = false;
    if (kind == MQTT_STORE_RECORD_END) {
        *done = true;
        if (snapshot) {
            scanner->end_at = offset;
            scanner->end_length = length;
        } else {
            status = store_damage(scanner, MQTT_STORE_FAULT_RECORD, offset,
                                  kind);
        }
    } else if (kind == MQTT_STORE_RECORD_COMMIT) {
        status = snapshot ? store_damage(scanner, MQTT_STORE_FAULT_RECORD,
                                         offset, kind) :
                            store_scan_commit(scanner, offset, length, after);
        scanner->scan->records += 1U;
    } else {
        scanner->scan->records += 1U;
    }
    return status;
}

static mqtt_status store_scan_frame(store_scanner *scanner, uint64_t offset,
                                    uint64_t *next, bool *done)
{
    /** @brief The record's kind. */
    const uint8_t kind = scanner->buffer[0];
    /** @brief Number of bytes of content. */
    const uint32_t length = store_get_u32(&scanner->buffer[1], 4U);
    /** @brief Offset of the record's CRC. */
    const uint64_t tail = offset + MQTT_STORE_FRAME_HEAD + (uint64_t)length;
    /** @brief The CRC of what has been read. */
    uint16_t crc = mqtt_store_frame_crc(scanner->buffer);
    /** @brief false if the place ends inside the record. */
    bool whole = false;
    /** @brief Number of bytes of the CRC read. */
    uint32_t count = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    *done = true;
    if (length > MQTT_STORE_CONTENT_MAX) {
        /* The head passed its check, so this is no record cut short. */
        status = store_damage(scanner, MQTT_STORE_FAULT_RECORD, offset, kind);
    } else {
        status = store_crc_content(scanner, offset + MQTT_STORE_FRAME_HEAD,
                                   length, &crc, &whole);
    }
    if ((status == MQTT_OK) && whole) {
        status = store_read(scanner, tail, MQTT_STORE_FRAME_TAIL, &count);
    }
    if ((status == MQTT_OK) && whole && (count == MQTT_STORE_FRAME_TAIL)) {
        if (store_get_u16(scanner->buffer) != crc) {
            status = store_scan_tail(scanner, offset,
                                     tail + MQTT_STORE_FRAME_TAIL, kind, true);
        } else {
            *next = tail + MQTT_STORE_FRAME_TAIL;
            status = store_scan_kind(scanner, offset, kind, length, done);
        }
    } else if ((status == MQTT_OK) && (length <= MQTT_STORE_CONTENT_MAX)) {
        /* The place ends inside the record. */
        scanner->scan->torn = true;
    } else {
        /* A length too large was judged, or the medium failed. */
    }
    return status;
}

static mqtt_status store_scan_record(store_scanner *scanner, uint64_t offset,
                                     uint64_t *next, bool *done)
{
    /** @brief Number of bytes of the kind and length read. */
    uint32_t count = 0U;
    /** @brief The outcome. */
    mqtt_status status = store_read(scanner, offset, MQTT_STORE_FRAME_HEAD,
                                    &count);

    *done = true;
    if ((status == MQTT_OK) && (count > 0U)) {
        /** @brief The first byte, which should be a kind of record. */
        const uint8_t kind = scanner->buffer[0];

        /** @brief The kind, as the head gives it. */
        uint8_t checked = 0U;
        /** @brief The length, as the head gives it. */
        uint32_t length = 0U;

        if ((count < MQTT_STORE_FRAME_HEAD) || (!store_kind_valid(kind))) {
            /** @brief true if every byte from here on is erased. */
            bool erased = false;

            status = store_erased_from(scanner, offset, &erased);
            if ((status == MQTT_OK) && (!erased)) {
                if (store_kind_valid(kind)) {
                    scanner->scan->torn = true;
                } else {
                    status = store_damage(scanner, MQTT_STORE_FAULT_RECORD,
                                          offset, kind);
                }
            }
        } else if (!mqtt_store_frame_check(scanner->buffer, &checked,
                                           &length)) {
            /* A head that fails its own check was cut short only if nothing
             * but erased bytes comes after it: its length can't be trusted
             * to say where the record would end. */
            status = store_scan_tail(scanner, offset,
                                     offset + MQTT_STORE_FRAME_HEAD, kind,
                                     true);
        } else {
            status = store_scan_frame(scanner, offset, next, done);
        }
    }
    return status;
}

static mqtt_status store_scan_header(store_scanner *scanner, uint8_t kind)
{
    /** @brief Number of bytes read. */
    uint32_t count = 0U;
    /** @brief true if the magic number and the CRC are right. */
    bool intact = false;
    /** @brief The outcome. */
    mqtt_status status = store_read(scanner, 0U, MQTT_STORE_HEADER_SIZE,
                                    &count);

    if ((status == MQTT_OK) && (count == MQTT_STORE_HEADER_SIZE)) {
        intact = store_header_intact(scanner->buffer);
    }
    if ((status == MQTT_OK) && intact) {
        /** @brief What is wrong with the header, if anything. */
        const mqtt_store_fault_kind found = mqtt_store_header_decode(
            scanner->buffer, kind, &scanner->scan->header);

        if (found != MQTT_STORE_FAULT_NONE) {
            status = store_damage(scanner, found, 0U, 0U);
        }
    } else if (status == MQTT_OK) {
        /** @brief true if every byte after the header is erased. */
        bool erased = true;

        if (count == MQTT_STORE_HEADER_SIZE) {
            status = store_erased_from(scanner, MQTT_STORE_HEADER_SIZE,
                                       &erased);
        }
        if (status == MQTT_OK) {
            status = erased ? MQTT_NOT_FOUND :
                              store_damage(scanner, MQTT_STORE_FAULT_HEADER,
                                           0U, 0U);
        }
    } else {
        /* The medium failed. */
    }
    return status;
}

static mqtt_status store_scan_records(store_scanner *scanner)
{
    /** @brief Offset of the record being checked. */
    uint64_t offset = MQTT_STORE_HEADER_SIZE;
    /** @brief true once the records end. */
    bool done = false;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    while ((!done) && (status == MQTT_OK)) {
        /** @brief Offset of the record after it. */
        uint64_t next = offset;

        status = store_scan_record(scanner, offset, &next, &done);
        offset = next;
    }
    if ((status == MQTT_OK) && (scanner->part == MQTT_STORE_SNAPSHOT)) {
        status = (scanner->end_at == 0U) ?
                     store_damage(scanner, MQTT_STORE_FAULT_RECORD, offset,
                                  0U) :
                     store_scan_end(scanner, scanner->end_at,
                                    scanner->end_length, offset);
    }
    return status;
}

static mqtt_status store_fail(mqtt_store_fault *fault,
                              mqtt_store_fault_kind kind)
{
    if (fault->kind == MQTT_STORE_FAULT_NONE) {
        fault->kind = kind;
        fault->part = MQTT_STORE_JOURNAL;
        fault->place = 0U;
        fault->generation = 0U;
        fault->offset = 0U;
        fault->record_kind = 0U;
    }
    return MQTT_STORE_DAMAGED;
}

static mqtt_status store_scan_all(const mqtt_store_source *source,
                                  uint8_t snapshot_kind, uint8_t journal_kind,
                                  uint8_t *buffer, uint32_t size,
                                  mqtt_store_place *copies,
                                  mqtt_store_place *journals,
                                  mqtt_store_fault *fault)
{
    /** @brief Index of the place. */
    uint32_t place = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    fault->kind = MQTT_STORE_FAULT_NONE;
    while ((status == MQTT_OK) && (place < 2U)) {
        journals[place].status = mqtt_store_scan_place(
            source, MQTT_STORE_JOURNAL, place, journal_kind, buffer, size,
            &journals[place].scan, &journals[place].fault);
        if (journals[place].status == MQTT_STORE_DAMAGED) {
            *fault = journals[place].fault;
            status = MQTT_STORE_DAMAGED;
        }
        place += 1U;
    }
    place = 0U;
    while ((status == MQTT_OK) && (place < source->copies)) {
        copies[place].status = mqtt_store_scan_place(
            source, MQTT_STORE_SNAPSHOT, place, snapshot_kind, buffer, size,
            &copies[place].scan, &copies[place].fault);
        place += 1U;
    }
    return status;
}

static mqtt_status store_follow(const mqtt_store_place *journals,
                                uint64_t generation, uint64_t base,
                                mqtt_store_chain *chain,
                                mqtt_store_fault *fault)
{
    /** @brief Index of the place. */
    uint32_t place = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_NOT_FOUND;

    chain->first = MQTT_STORE_PLACE_NONE;
    chain->second = MQTT_STORE_PLACE_NONE;
    while (place < 2U) {
        if ((journals[place].status == MQTT_OK) &&
            (journals[place].scan.header.generation == generation) &&
            (journals[place].scan.header.base == base)) {
            chain->first = place;
            status = MQTT_OK;
        }
        place += 1U;
    }
    if (status == MQTT_OK) {
        /** @brief The other place. */
        const mqtt_store_place *other = &journals[1U - chain->first];
        /** @brief The first segment. */
        const mqtt_store_scan *first = &journals[chain->first].scan;

        if ((other->status != MQTT_OK) ||
            (other->scan.header.generation < generation)) {
            /* An older segment, which this one replaces. */
        } else if (other->scan.header.generation != (generation + 1U)) {
            status = store_fail(fault, MQTT_STORE_FAULT_CHAIN);
        } else if ((other->scan.header.flags & MQTT_STORE_FLAG_RECOVERY) !=
                   0U) {
            /* A segment after a gap counts only with its own snapshot. */
        } else if ((!first->torn) &&
                   (other->scan.header.base == (base + first->commits))) {
            chain->second = 1U - chain->first;
        } else {
            status = store_fail(fault, MQTT_STORE_FAULT_CHAIN);
        }
    }
    return status;
}
