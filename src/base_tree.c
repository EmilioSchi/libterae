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
 * @file base_tree.c
 * @brief The tree of topic levels that holds subscriptions and retained
 * messages.
 *
 * Wildcard levels hang off their parent through single_child and multi_child,
 * and are in no trie and no circle. So every node reached through children or
 * next is a level of text, which may be empty, and a wildcard node's next is
 * always MQTT_TREE_NONE.
 *
 * The children of a node whose levels are text are the leaves of a crit-bit
 * trie. A level is read as a run of nine-bit symbols, each byte with a ninth
 * bit set and zeros past its end, and a branch tests the first bit at which the
 * children below it differ, in the order of a position: by byte, and in a byte
 * the ninth bit first, then from the highest. Positions grow down every path. A
 * search stops as soon as a branch tests a bit past the end of the level it
 * looks for, as the children below all read alike up to that bit, so a level of
 * L bytes takes at most 9 L + 1 branches whatever the other levels are. Adding
 * a child follows its bits to some child, finds the first bit where the two
 * differ, and puts a branch at that bit's place on the path.
 *
 * Each branch names a node below it, its sample. The sample of the branch at
 * the top is the first of the circle of children, which is kept in the order
 * they were added, and a branch put in takes the sample of what it goes over,
 * so the first stays first until it goes.
 *
 * A node comes from the tree's slab, which keeps the nodes in use in as few
 * pages as it can, and stays where it is until it is removed.
 *
 * A page of text holds whole records, packed from its start, and the count word
 * of its metadata says how many bytes. A record is the index of a level's node,
 * high byte first, then the level's text, and the node holds where the text
 * starts. A new record goes to the end of the page last used, or to a new page,
 * or failing both to the first page with room. When a level goes, the records
 * after it in its page move down over it, and each one names the node to tell
 * where its text went, so that takes one page and no search of the nodes. A
 * record longer than a page has a run of pages to itself, which is how a page
 * and such a run are told apart.
 *
 * A match and a walk change nothing.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 4.7, p.102-105.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */

#include <stddef.h>

#include "base_string.h"
#include "base_topic.h"
#include "base_tree.h"
#include "v5_topic.h"

/** @brief The root of the tree. */
#define TREE_ROOT 0U
/** @brief The offset that stands for no text. */
#define TREE_TEXT_NONE 0xFFFFFFFFU
/** @brief The ninth bit of a symbol, which says the level has that byte. */
#define TREE_SYMBOL_THERE 0x100U
/** @brief The bit of a branch that tests the ninth bit. */
#define TREE_BIT_THERE 8U
/** @brief The shift of a byte's index in a position. */
#define TREE_POSITION_SHIFT 4U
/** @brief The bits of a position below its byte's index. */
#define TREE_POSITION_BITS 0xFU
/** @brief The position that stands for none. */
#define TREE_POSITION_NONE 0xFFFFFFFFU
/** @brief The bit of a node's kinds that says its children link is a node. */
#define TREE_KIND_NODE 1U

/**
 * @brief Find the next level of a topic.
 * @param[in] data The bytes of the topic.
 * @param[in] length Number of bytes.
 * @param[in] start Index where the level starts.
 * @return Index of the next separator, or length if there is none.
 */
static uint32_t tree_level_end(const uint8_t *data, uint32_t length,
                               uint32_t start)
{
    /** @brief Index being looked at. */
    uint32_t end = start;

    while ((end < length) &&
           ((uint32_t)data[end] != MQTT_TOPIC_LEVEL_SEPARATOR)) {
        end += 1U;
    }
    return end;
}

/**
 * @brief Decide whether a node holds the text of a level.
 * @param[in] tree The tree.
 * @param[in] node The node.
 * @param[in] data The level's text.
 * @param[in] length Number of bytes in the text.
 * @return true if it does.
 */
static bool tree_same_text(const mqtt_tree *tree, const mqtt_tree_node *node,
                           const uint8_t *data, uint32_t length)
{
    return ((uint32_t)node->text_length == length) &&
           ((length == 0U) ||
            mqtt_bytes_equal(&tree->text[node->text_offset], data, length));
}

/**
 * @brief Read a byte of a level as the trie does: with a ninth bit set, which
 * says the level has that byte, or as zero past the level's end.
 * @param[in] data The level's text.
 * @param[in] length Number of bytes in the text.
 * @param[in] index Index of the byte.
 * @return The byte with its ninth bit, or zero.
 */
static uint32_t tree_symbol(const uint8_t *data, uint32_t length,
                            uint32_t index)
{
    /** @brief The answer. */
    uint32_t symbol = 0U;

    if (index < length) {
        symbol = TREE_SYMBOL_THERE | (uint32_t)data[index];
    }
    return symbol;
}

/**
 * @brief Find where a branch's bit comes among the bits of a level: by byte,
 * and in a byte the ninth bit first, then from the highest.
 * @param[in] branch The branch.
 * @return The position, smaller for a bit read sooner.
 */
static uint32_t tree_position(const mqtt_tree_branch *branch)
{
    return ((uint32_t)branch->byte << TREE_POSITION_SHIFT) |
           (TREE_BIT_THERE - (uint32_t)branch->bit);
}

/**
 * @brief Find the side of a branch a level goes.
 * @param[in] branch The branch.
 * @param[in] data The level's text.
 * @param[in] length Number of bytes in the text.
 * @return 0 if the bit it tests is clear, 1 if it is set.
 */
static uint32_t tree_side(const mqtt_tree_branch *branch, const uint8_t *data,
                          uint32_t length)
{
    return (tree_symbol(data, length, (uint32_t)branch->byte) >>
            (uint32_t)branch->bit) &
           1U;
}

/**
 * @brief Find a node an item of a trie leads to: the item itself if it is a
 * node, or else its branch's sample.
 * @param[in] tree The tree.
 * @param[in] item A node or a branch.
 * @param[in] node true if the item is a node.
 * @return The node.
 */
static uint32_t tree_sample(const mqtt_tree *tree, uint32_t item, bool node)
{
    return node ? item : (uint32_t)tree->branches[item].sample;
}

/**
 * @brief Find the first in the circle of a node's children whose levels are
 * text: the only one, or the sample of the branch at the top of their trie.
 * @param[in] tree The tree.
 * @param[in] parent The parent node.
 * @return The child, or MQTT_TREE_NONE if there are none.
 */
static uint32_t tree_first_child(const mqtt_tree *tree, uint32_t parent)
{
    /** @brief The parent. */
    const mqtt_tree_node *above = &tree->nodes[parent];
    /** @brief The child. */
    uint32_t first = MQTT_TREE_NONE;

    if (above->children != MQTT_TREE_NONE) {
        first = tree_sample(tree, (uint32_t)above->children,
                            (((uint32_t)above->kinds & TREE_KIND_NODE) != 0U));
    }
    return first;
}

/**
 * @brief Follow a level's bits down the trie of a node's children, to the child
 * they lead to, or to a branch that tests a bit past the level's end. Every
 * child below such a branch reads as the others do up to its bit, so if the
 * level were one of them they would all end where it ends, and be the same.
 * @param[in] tree The tree.
 * @param[in] parent The parent node.
 * @param[in] data The level's text.
 * @param[in] length Number of bytes in the text.
 * @return The child reached, or the sample of the branch the walk stopped at,
 * or MQTT_TREE_NONE if the parent has no children whose levels are text.
 */
static uint32_t tree_trie_candidate(const mqtt_tree *tree, uint32_t parent,
                                    const uint8_t *data, uint32_t length)
{
    /** @brief The parent. */
    const mqtt_tree_node *above = &tree->nodes[parent];
    /** @brief The position of the bit that says whether the level ends. */
    const uint32_t end = length << TREE_POSITION_SHIFT;
    /** @brief The item reached. */
    uint32_t item = (uint32_t)above->children;
    /** @brief true if it is a node. */
    bool node = (((uint32_t)above->kinds & TREE_KIND_NODE) != 0U);
    /** @brief The child found. */
    uint32_t candidate = MQTT_TREE_NONE;

    while ((candidate == MQTT_TREE_NONE) && (item != MQTT_TREE_NONE)) {
        if (node) {
            candidate = item;
        } else {
            /** @brief The branch. */
            const mqtt_tree_branch *branch = &tree->branches[item];

            if (tree_position(branch) > end) {
                candidate = (uint32_t)branch->sample;
            } else {
                /** @brief The side the level goes. */
                const uint32_t side = tree_side(branch, data, length);

                item = (uint32_t)branch->child[side];
                node = ((((uint32_t)branch->kinds >> side) & 1U) != 0U);
            }
        }
    }
    return candidate;
}

/**
 * @brief Find a node's child for a level's text.
 * @param[in] tree The tree.
 * @param[in] parent The parent node.
 * @param[in] data The level's text.
 * @param[in] length Number of bytes in the text.
 * @return The child, or MQTT_TREE_NONE.
 */
static uint32_t tree_trie_find(const mqtt_tree *tree, uint32_t parent,
                               const uint8_t *data, uint32_t length)
{
    /** @brief The child. */
    uint32_t child = tree_trie_candidate(tree, parent, data, length);

    if ((child != MQTT_TREE_NONE) &&
        (!tree_same_text(tree, &tree->nodes[child], data, length))) {
        child = MQTT_TREE_NONE;
    }
    return child;
}

/**
 * @brief Find the first bit at which a level differs from a node's.
 * @param[in] tree The tree.
 * @param[in] node The node.
 * @param[in] data The level's text.
 * @param[in] length Number of bytes in the text.
 * @return The bit's position, or TREE_POSITION_NONE if the level is the node's.
 */
static uint32_t tree_difference(const mqtt_tree *tree, uint32_t node,
                                const uint8_t *data, uint32_t length)
{
    /** @brief The node. */
    const mqtt_tree_node *entry = &tree->nodes[node];
    /** @brief The node's text. */
    const uint8_t *other = &tree->text[entry->text_offset];
    /** @brief Number of bytes in it. */
    const uint32_t other_length = (uint32_t)entry->text_length;
    /** @brief Index of the byte being compared. */
    uint32_t index = 0U;
    /** @brief The bits in which the two symbols differ. */
    uint32_t differ = 0U;
    /** @brief The answer. */
    uint32_t position = TREE_POSITION_NONE;

    /* Two levels of different lengths differ by the shorter one's end. */
    while ((differ == 0U) && (index <= length) && (index <= other_length)) {
        differ = tree_symbol(data, length, index) ^
                 tree_symbol(other, other_length, index);
        if (differ == 0U) {
            index += 1U;
        }
    }
    if (differ != 0U) {
        /** @brief The highest bit in which they differ. */
        uint32_t bit = TREE_BIT_THERE;

        while (((differ >> bit) & 1U) == 0U) {
            bit -= 1U;
        }
        position = (index << TREE_POSITION_SHIFT) | (TREE_BIT_THERE - bit);
    }
    return position;
}

/**
 * @brief Take a free node and clear it.
 * @param[in,out] tree The tree.
 * @param[in] parent The node above the new one.
 * @return The node, or MQTT_TREE_NONE if there are no more.
 */
static uint32_t tree_node_take(mqtt_tree *tree, uint32_t parent)
{
    /** @brief The node's slot. */
    const uint32_t slot = mqtt_slab_take(&tree->node_slots);
    /** @brief The node. */
    uint32_t index = MQTT_TREE_NONE;

    if (slot != MQTT_SLAB_NONE) {
        /** @brief The node. */
        mqtt_tree_node *node = &tree->nodes[slot];

        index = slot;
        node->subscriptions = MQTT_TREE_UNUSED;
        node->groups = MQTT_TREE_UNUSED;
        node->retained = MQTT_TREE_UNUSED;
        node->text_offset = 0U;
        node->text_length = 0U;
        node->parent = (uint16_t)parent;
        node->children = MQTT_TREE_NONE;
        node->next = MQTT_TREE_NONE;
        node->previous = MQTT_TREE_NONE;
        node->single_child = MQTT_TREE_NONE;
        node->multi_child = MQTT_TREE_NONE;
        node->kinds = 0U;
    }
    return index;
}

/**
 * @brief Find the count of bytes in use of a page of text.
 * @param[in] tree The tree.
 * @param[in] page The page, which holds levels packed from its start.
 * @return The count.
 */
static uint32_t *tree_page_used(const mqtt_tree *tree, uint32_t page)
{
    /** @brief The page's first word of metadata. */
    const uint32_t first = page * MQTT_PAGE_META_WORDS;

    return &tree->text_pages.meta[first + MQTT_PAGE_WORD_COUNT];
}

/**
 * @brief Find the first page of text with room for a record, other than the
 * page new records go to first.
 * @param[in] tree The tree.
 * @param[in] length Number of bytes in the record, at most a page.
 * @return The page, or MQTT_PAGE_NONE.
 */
static uint32_t tree_page_with_room(const mqtt_tree *tree, uint32_t length)
{
    /** @brief The page being looked at. */
    uint32_t candidate = 0U;
    /** @brief The page found. */
    uint32_t page = MQTT_PAGE_NONE;

    while ((page == MQTT_PAGE_NONE) &&
           (candidate < tree->text_pages.page_count)) {
        /* A run of one page holds levels packed; a longer run holds one. */
        if ((mqtt_runs_length(&tree->text_pages, candidate) == 1U) &&
            ((*tree_page_used(tree, candidate) + length) <= MQTT_PAGE_SIZE)) {
            page = candidate;
        }
        candidate += 1U;
    }
    return page;
}

/**
 * @brief Find a page of text with room for a record: the one new records go to
 * first, or else a free page, or else the first with room.
 * @param[in,out] tree The tree.
 * @param[in] length Number of bytes in the record, at most a page.
 * @return The page, or MQTT_PAGE_NONE.
 */
static uint32_t tree_text_page(mqtt_tree *tree, uint32_t length)
{
    /** @brief The page. */
    uint32_t page = tree->text_page;

    if ((page == MQTT_PAGE_NONE) ||
        ((*tree_page_used(tree, page) + length) > MQTT_PAGE_SIZE)) {
        page = mqtt_runs_take(&tree->text_pages, 1U);
        if (page != MQTT_PAGE_NONE) {
            *tree_page_used(tree, page) = 0U;
        } else {
            page = tree_page_with_room(tree, length);
        }
        if (page != MQTT_PAGE_NONE) {
            tree->text_page = page;
        }
    }
    return page;
}

/**
 * @brief Keep a level's record, its node's index and then its text: at the end
 * of a page with room, or in a run of pages of its own if it is longer than a
 * page.
 * @param[in,out] tree The tree.
 * @param[in] data The level's text.
 * @param[in] length Number of bytes in the text, at least one.
 * @param[in] node The level's node.
 * @return Where the text starts, or TREE_TEXT_NONE if there is no room.
 */
static uint32_t tree_text_take(mqtt_tree *tree, const uint8_t *data,
                               uint32_t length, uint32_t node)
{
    /** @brief Number of bytes in the record. */
    const uint32_t size = length + MQTT_TREE_TEXT_HEAD;
    /** @brief Where the record starts. */
    uint32_t record = TREE_TEXT_NONE;
    /** @brief Where the text starts. */
    uint32_t offset = TREE_TEXT_NONE;

    if (size <= MQTT_PAGE_SIZE) {
        /** @brief The page. */
        const uint32_t page = tree_text_page(tree, size);

        if (page != MQTT_PAGE_NONE) {
            /** @brief The page's count of bytes in use. */
            uint32_t *used = tree_page_used(tree, page);

            record = (page * MQTT_PAGE_SIZE) + *used;
            *used += size;
        }
    } else {
        /** @brief The first page of a run long enough. */
        const uint32_t first = mqtt_runs_take(
            &tree->text_pages, (size + (MQTT_PAGE_SIZE - 1U)) / MQTT_PAGE_SIZE);

        if (first != MQTT_PAGE_NONE) {
            record = first * MQTT_PAGE_SIZE;
        }
    }
    if (record != TREE_TEXT_NONE) {
        offset = record + MQTT_TREE_TEXT_HEAD;
        tree->text[record] = (uint8_t)(node >> 8U);
        tree->text[record + 1U] = (uint8_t)(node & 0xFFU);
        /* The room was found above, so it fits. */
        mqtt_bytes_put(&tree->text[offset], data, length);
        tree->text_used += length;
    }
    return offset;
}

/**
 * @brief Read the node a record names.
 * @param[in] tree The tree.
 * @param[in] record Where the record starts.
 * @return The node.
 */
static uint32_t tree_record_node(const mqtt_tree *tree, uint32_t record)
{
    return ((uint32_t)tree->text[record] << 8U) |
           (uint32_t)tree->text[record + 1U];
}

/**
 * @brief Move the records after one down over it, within its page, and tell
 * the node each one names where its text now starts.
 * @param[in,out] tree The tree.
 * @param[in] record Where the record that goes starts.
 * @param[in] size Number of bytes in it.
 * @param[in] end Where the records in use of its page end.
 */
static void tree_text_close(mqtt_tree *tree, uint32_t record, uint32_t size,
                            uint32_t end)
{
    /** @brief Index of the byte being moved, then of the record. */
    uint32_t index = record + size;

    /* The two ranges overlap, so this stays a forward byte loop:
     * mqtt_bytes_put may be memcpy, which needs them apart. */
    while (index < end) {
        tree->text[index - size] = tree->text[index];
        index += 1U;
    }
    index = record;
    while (index < (end - size)) {
        /** @brief The node the record names. */
        mqtt_tree_node *node = &tree->nodes[tree_record_node(tree, index)];

        node->text_offset = index + MQTT_TREE_TEXT_HEAD;
        index += MQTT_TREE_TEXT_HEAD + (uint32_t)node->text_length;
    }
}

/**
 * @brief Give back a level's record.
 * @param[in,out] tree The tree.
 * @param[in] offset Where the text starts.
 * @param[in] length Number of bytes in the text, which may be zero, and then
 * there is no record.
 */
static void tree_text_free(mqtt_tree *tree, uint32_t offset, uint32_t length)
{
    if (length > 0U) {
        /** @brief Where the record starts. */
        const uint32_t record = offset - MQTT_TREE_TEXT_HEAD;
        /** @brief Number of bytes in the record. */
        const uint32_t size = length + MQTT_TREE_TEXT_HEAD;
        /** @brief The page the record starts in. */
        const uint32_t page = record / MQTT_PAGE_SIZE;

        if (size > MQTT_PAGE_SIZE) {
            mqtt_runs_give(&tree->text_pages, page);
        } else {
            /** @brief The page's count of bytes in use. */
            uint32_t *used = tree_page_used(tree, page);
            /** @brief Where the records in use of the page end. */
            const uint32_t end = (page * MQTT_PAGE_SIZE) + *used;

            if ((record + size) < end) {
                tree_text_close(tree, record, size, end);
            }
            *used -= size;
            if (*used == 0U) {
                mqtt_runs_give(&tree->text_pages, page);
                if (tree->text_page == page) {
                    tree->text_page = MQTT_PAGE_NONE;
                }
            }
        }
        tree->text_used -= length;
    }
}

/**
 * @brief Take a branch: the one given back last, or else one never used.
 * @param[in,out] tree The tree.
 * @return The branch, or MQTT_TREE_NONE if there are no more.
 */
static uint32_t tree_branch_take(mqtt_tree *tree)
{
    /** @brief The branch. */
    uint32_t branch = tree->branch_free;

    if (branch != MQTT_TREE_NONE) {
        tree->branch_free = (uint32_t)tree->branches[branch].child[0];
    } else if (tree->branch_high < tree->branch_count) {
        branch = tree->branch_high;
        tree->branch_high += 1U;
    } else {
        /* Every branch is in use, which a pool as large as the set-up asks for
         * never is. */
    }
    return branch;
}

/**
 * @brief Give a branch back.
 * @param[in,out] tree The tree.
 * @param[in] branch The branch, which nothing links to any more.
 */
static void tree_branch_give(mqtt_tree *tree, uint32_t branch)
{
    tree->branches[branch].child[0] = (uint16_t)tree->branch_free;
    tree->branch_free = branch;
}

/**
 * @brief Put a new branch in the trie of a node's children, at the place of
 * its bit on the path of a new child's level, with the child on one side and
 * what was there on the other. The branch takes the sample of what it goes
 * over, so the first child stays first.
 * @param[in,out] tree The tree.
 * @param[in] parent The parent node, which has children whose levels are text.
 * @param[in] child The new child, whose text is kept, and which is in no trie.
 * @param[in] branch The new branch.
 * @param[in] position The first bit at which the child's level differs from
 * the child its bits led to.
 */
static void tree_trie_splice(mqtt_tree *tree, uint32_t parent, uint32_t child,
                             uint32_t branch, uint32_t position)
{
    /** @brief The child's text. */
    const uint8_t *data = &tree->text[tree->nodes[child].text_offset];
    /** @brief Number of bytes in it. */
    const uint32_t length = (uint32_t)tree->nodes[child].text_length;
    /** @brief The new branch. */
    mqtt_tree_branch *fresh = &tree->branches[branch];
    /** @brief The link being followed. */
    uint16_t *link = &tree->nodes[parent].children;
    /** @brief The byte that says what the link leads to. */
    uint8_t *kinds = &tree->nodes[parent].kinds;
    /** @brief The bit of that byte for the link. */
    uint32_t mask = TREE_KIND_NODE;
    /** @brief true if what the link leads to is a node. */
    bool node = false;
    /** @brief A side of a branch. */
    uint32_t side = 0U;

    while ((((uint32_t)*kinds & mask) == 0U) &&
           (tree_position(&tree->branches[*link]) < position)) {
        /** @brief The branch passed. */
        mqtt_tree_branch *passed = &tree->branches[*link];

        side = tree_side(passed, data, length);
        link = &passed->child[side];
        kinds = &passed->kinds;
        mask = (uint32_t)1U << side;
    }
    node = (((uint32_t)*kinds & mask) != 0U);
    fresh->byte = (uint16_t)(position >> TREE_POSITION_SHIFT);
    fresh->bit = (uint8_t)(TREE_BIT_THERE - (position & TREE_POSITION_BITS));
    side = tree_side(fresh, data, length);
    fresh->child[side] = (uint16_t)child;
    fresh->child[1U - side] = *link;
    fresh->kinds = (uint8_t)((uint32_t)1U << side);
    if (node) {
        fresh->kinds =
            (uint8_t)((uint32_t)fresh->kinds | ((uint32_t)1U << (1U - side)));
    }
    fresh->sample = (uint16_t)tree_sample(tree, (uint32_t)*link, node);
    *link = (uint16_t)branch;
    *kinds = (uint8_t)((uint32_t)*kinds & ~mask);
}

/**
 * @brief Put a new child last in the circle of its parent's children: just
 * before the first, or alone.
 * @param[in,out] tree The tree.
 * @param[in] parent The parent node.
 * @param[in] child The child, which is in the trie, and first only if alone.
 */
static void tree_circle_join(mqtt_tree *tree, uint32_t parent, uint32_t child)
{
    /** @brief The first child. */
    const uint32_t first = tree_first_child(tree, parent);
    /** @brief The new child. */
    mqtt_tree_node *entry = &tree->nodes[child];

    if (first == child) {
        entry->next = (uint16_t)child;
        entry->previous = (uint16_t)child;
    } else {
        /** @brief The child that was last. */
        const uint32_t last = (uint32_t)tree->nodes[first].previous;

        entry->next = (uint16_t)first;
        entry->previous = (uint16_t)last;
        tree->nodes[last].next = (uint16_t)child;
        tree->nodes[first].previous = (uint16_t)child;
    }
}

/**
 * @brief Take a child out of the circle of its parent's children.
 * @param[in,out] tree The tree.
 * @param[in] child The child.
 */
static void tree_circle_leave(mqtt_tree *tree, uint32_t child)
{
    /** @brief The child. */
    const mqtt_tree_node *entry = &tree->nodes[child];

    tree->nodes[entry->previous].next = entry->next;
    tree->nodes[entry->next].previous = entry->previous;
}

/**
 * @brief Add a child for a level's text: a node, its record, and a branch if
 * the parent has children whose levels are text, all or none.
 * @param[in,out] tree The tree.
 * @param[in] parent The parent node.
 * @param[in] data The level's text.
 * @param[in] length Number of bytes in the text.
 * @param[in] position The first bit at which the level differs from the child
 * its bits lead to, or TREE_POSITION_NONE if the parent has no such children.
 * @return The child, or MQTT_TREE_NONE if there is no room.
 */
static uint32_t tree_add_child(mqtt_tree *tree, uint32_t parent,
                               const uint8_t *data, uint32_t length,
                               uint32_t position)
{
    /** @brief Where its text starts, if it has any. */
    uint32_t offset = 0U;
    /** @brief The branch, if one is needed. */
    uint32_t branch = MQTT_TREE_NONE;
    /** @brief The new node, taken first, as its record names it. */
    uint32_t index = tree_node_take(tree, parent);
    /** @brief true while all that is needed has been had. */
    bool good = (index != MQTT_TREE_NONE);

    if (good && (length > 0U)) {
        offset = tree_text_take(tree, data, length, index);
        good = (offset != TREE_TEXT_NONE);
    }
    if (good && (position != TREE_POSITION_NONE)) {
        branch = tree_branch_take(tree);
        good = (branch != MQTT_TREE_NONE);
        if (!good) {
            tree_text_free(tree, offset, length);
        }
    }
    if (good) {
        tree->nodes[index].text_offset = offset;
        tree->nodes[index].text_length = (uint16_t)length;
        if (branch == MQTT_TREE_NONE) {
            tree->nodes[parent].children = (uint16_t)index;
            tree->nodes[parent].kinds = (uint8_t)TREE_KIND_NODE;
        } else {
            tree_trie_splice(tree, parent, index, branch, position);
        }
        tree_circle_join(tree, parent, index);
    } else if (index != MQTT_TREE_NONE) {
        (void)mqtt_slab_give(&tree->node_slots, index);
        index = MQTT_TREE_NONE;
    } else {
        /* There was no node to give back. */
    }
    return index;
}

/**
 * @brief Find a node's child for a level's text, adding it if it is missing.
 * @param[in,out] tree The tree.
 * @param[in] parent The parent node.
 * @param[in] data The level's text.
 * @param[in] length Number of bytes in the text.
 * @return The child, or MQTT_TREE_NONE if there is no room.
 */
static uint32_t tree_child_add(mqtt_tree *tree, uint32_t parent,
                               const uint8_t *data, uint32_t length)
{
    /** @brief The child the level's bits lead to. */
    uint32_t child = tree_trie_candidate(tree, parent, data, length);
    /** @brief The first bit at which the level differs from it. */
    uint32_t position = TREE_POSITION_NONE;

    if (child != MQTT_TREE_NONE) {
        position = tree_difference(tree, child, data, length);
    }
    if ((child == MQTT_TREE_NONE) || (position != TREE_POSITION_NONE)) {
        child = tree_add_child(tree, parent, data, length, position);
    }
    return child;
}

/**
 * @brief Put the other side of a branch in its place, and give the branch
 * back. A branch at the top that goes leaves its sample, the first child, to
 * the branch that takes its place.
 * @param[in,out] tree The tree.
 * @param[in] top The link at the top of the trie.
 * @param[in] link The link to the branch.
 * @param[in] kinds The byte that says what the link leads to.
 * @param[in] mask The bit of that byte for the link.
 * @param[in] side The side of the branch that goes.
 */
static void tree_trie_lift(mqtt_tree *tree, const uint16_t *top,
                           uint16_t *link, uint8_t *kinds, uint32_t mask,
                           uint32_t side)
{
    /** @brief The branch. */
    const uint32_t gone = (uint32_t)*link;
    /** @brief Its contents. */
    const mqtt_tree_branch *branch = &tree->branches[gone];
    /** @brief The side that stays. */
    const uint32_t other = 1U - side;
    /** @brief What is on that side. */
    const uint32_t kept = (uint32_t)branch->child[other];
    /** @brief true if it is a node. */
    const bool node = ((((uint32_t)branch->kinds >> other) & 1U) != 0U);

    if ((link == top) && (!node)) {
        tree->branches[kept].sample = branch->sample;
    }
    *link = (uint16_t)kept;
    if (node) {
        *kinds = (uint8_t)((uint32_t)*kinds | mask);
    } else {
        *kinds = (uint8_t)((uint32_t)*kinds & ~mask);
    }
    tree_branch_give(tree, gone);
}

/**
 * @brief Take a child out of the trie of its parent's children. Each branch on
 * its path whose sample it is gets another: at the top the next child in the
 * circle, which then comes first, and further down one from the other side.
 * @param[in,out] tree The tree.
 * @param[in] parent The parent node.
 * @param[in] child The child, whose record is still in the pages of text.
 */
static void tree_trie_remove(mqtt_tree *tree, uint32_t parent, uint32_t child)
{
    /** @brief The child. */
    const mqtt_tree_node *entry = &tree->nodes[child];
    /** @brief Its text. */
    const uint8_t *data = &tree->text[entry->text_offset];
    /** @brief The link at the top of the trie. */
    uint16_t *top = &tree->nodes[parent].children;
    /** @brief The link being followed, and the one to the branch above it. */
    uint16_t *link[2] = {top, NULL};
    /** @brief The bytes that say what each of those leads to. */
    uint8_t *kinds[2] = {&tree->nodes[parent].kinds, NULL};
    /** @brief The bits of those bytes for the links. */
    uint32_t mask[2] = {TREE_KIND_NODE, 0U};
    /** @brief The side the child is on, at the last branch passed. */
    uint32_t side = 0U;

    while (((uint32_t)*kinds[0] & mask[0]) == 0U) {
        /** @brief The branch passed. */
        mqtt_tree_branch *branch = &tree->branches[*link[0]];
        /** @brief The other side. */
        uint32_t other = 0U;

        side = tree_side(branch, data, (uint32_t)entry->text_length);
        other = 1U - side;
        if ((uint32_t)branch->sample == child) {
            branch->sample = (link[0] == top) ?
                                 entry->next :
                                 (uint16_t)tree_sample(
                                     tree, (uint32_t)branch->child[other],
                                     ((((uint32_t)branch->kinds >> other) &
                                       1U) != 0U));
        }
        link[1] = link[0];
        kinds[1] = kinds[0];
        mask[1] = mask[0];
        link[0] = &branch->child[side];
        kinds[0] = &branch->kinds;
        mask[0] = (uint32_t)1U << side;
    }
    if (link[1] == NULL) {
        *top = MQTT_TREE_NONE;
        tree->nodes[parent].kinds = 0U;
    } else {
        tree_trie_lift(tree, top, link[1], kinds[1], mask[1], side);
    }
}

/**
 * @brief Take a node out of its parent: off a wildcard link, or out of the
 * trie and the circle of the parent's children.
 * @param[in,out] tree The tree.
 * @param[in] index The node, which is not the root.
 */
static void tree_unlink(mqtt_tree *tree, uint32_t index)
{
    /** @brief The parent. */
    const uint32_t parent = (uint32_t)tree->nodes[index].parent;
    /** @brief Its contents. */
    mqtt_tree_node *above = &tree->nodes[parent];

    if ((uint32_t)above->single_child == index) {
        above->single_child = MQTT_TREE_NONE;
    } else if ((uint32_t)above->multi_child == index) {
        above->multi_child = MQTT_TREE_NONE;
    } else {
        tree_trie_remove(tree, parent, index);
        tree_circle_leave(tree, index);
    }
}

/**
 * @brief Find or add a child for one level.
 * @param[in,out] tree The tree.
 * @param[in] parent The parent node.
 * @param[in] data The level's text.
 * @param[in] length Number of bytes in the text.
 * @return The child, or MQTT_TREE_NONE if there is no room.
 */
static uint32_t tree_step_insert(mqtt_tree *tree, uint32_t parent,
                                 const uint8_t *data, uint32_t length)
{
    /** @brief The child. */
    uint32_t child = MQTT_TREE_NONE;

    if ((length == 1U) && ((uint32_t)data[0] == MQTT_SINGLE_LEVEL_WILDCARD)) {
        child = (uint32_t)tree->nodes[parent].single_child;
        if (child == MQTT_TREE_NONE) {
            child = tree_node_take(tree, parent);
            tree->nodes[parent].single_child = (uint16_t)child;
        }
    } else if ((length == 1U) &&
               ((uint32_t)data[0] == MQTT_MULTI_LEVEL_WILDCARD)) {
        child = (uint32_t)tree->nodes[parent].multi_child;
        if (child == MQTT_TREE_NONE) {
            child = tree_node_take(tree, parent);
            tree->nodes[parent].multi_child = (uint16_t)child;
        }
    } else {
        child = tree_child_add(tree, parent, data, length);
    }
    return child;
}

/**
 * @brief Find the child for one level.
 * @param[in] tree The tree.
 * @param[in] parent The parent node.
 * @param[in] data The level's text.
 * @param[in] length Number of bytes in the text.
 * @return The child, or MQTT_TREE_NONE.
 */
static uint32_t tree_step_find(const mqtt_tree *tree, uint32_t parent,
                               const uint8_t *data, uint32_t length)
{
    /** @brief The child. */
    uint32_t child = MQTT_TREE_NONE;

    if ((length == 1U) && ((uint32_t)data[0] == MQTT_SINGLE_LEVEL_WILDCARD)) {
        child = (uint32_t)tree->nodes[parent].single_child;
    } else if ((length == 1U) &&
               ((uint32_t)data[0] == MQTT_MULTI_LEVEL_WILDCARD)) {
        child = (uint32_t)tree->nodes[parent].multi_child;
    } else {
        child = tree_trie_find(tree, parent, data, length);
    }
    return child;
}

mqtt_status mqtt_tree_init(mqtt_tree *tree, const mqtt_tree_storage *storage)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if ((tree == NULL) || (storage == NULL) || (storage->nodes == NULL) ||
        (storage->node_meta == NULL) ||
        (storage->text == NULL) || (storage->text_meta == NULL) ||
        (storage->node_count == 0U) ||
        (storage->node_count > MQTT_TREE_NODES_MAX) ||
        (storage->text_pages > MQTT_PAGE_MAX_PAGES) ||
        (storage->branch_count > MQTT_TREE_NODES_MAX) ||
        ((storage->branch_count + 2U) < storage->node_count) ||
        ((storage->branches == NULL) && (storage->branch_count != 0U))) {
        status = MQTT_INVALID_ARGUMENT;
    } else {
        status = mqtt_slab_init_all(&tree->node_slots, storage->node_meta,
                                    MQTT_TREE_PAGE_NODES, storage->node_count);
    }
    if (status == MQTT_OK) {
        status = mqtt_runs_init(&tree->text_pages, storage->text_meta,
                                storage->text_pages);
    }
    if (status == MQTT_OK) {
        tree->nodes = storage->nodes;
        tree->text = storage->text;
        tree->text_page = MQTT_PAGE_NONE;
        tree->text_used = 0U;
        tree->branches = storage->branches;
        tree->branch_count = storage->branch_count;
        tree->branch_free = MQTT_TREE_NONE;
        tree->branch_high = 0U;
        /* The first slot of an empty slab is slot 0, the root. */
        (void)tree_node_take(tree, MQTT_TREE_NONE);
    }
    return status;
}

/**
 * @brief Do what mqtt_tree_insert does, for arguments known to be valid.
 * @param[in,out] tree The tree.
 * @param[in] topic A Topic Filter or Topic Name that has been checked.
 * @param[out] node The node.
 * @return MQTT_OK or MQTT_QUOTA_EXCEEDED, in which case nothing has been added.
 */
static mqtt_status tree_insert_body(mqtt_tree *tree, const mqtt_utf8 *topic,
                                    uint32_t *node)
{
    /** @brief Number of bytes in the topic. */
    const uint32_t length = (uint32_t)topic->length;
    /** @brief The node reached. */
    uint32_t current = TREE_ROOT;
    /** @brief Index where the level starts. */
    uint32_t start = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    while ((status == MQTT_OK) && (start <= length)) {
        /** @brief Index where the level ends. */
        const uint32_t end = tree_level_end(topic->data, length, start);
        /** @brief The child for this level. */
        const uint32_t child =
            tree_step_insert(tree, current, &topic->data[start], end - start);

        if (child == MQTT_TREE_NONE) {
            status = MQTT_QUOTA_EXCEEDED;
        } else {
            current = child;
            start = end + 1U;
        }
    }
    if (status == MQTT_OK) {
        *node = current;
    } else {
        mqtt_tree_prune(tree, current);
    }
    return status;
}

mqtt_status mqtt_tree_insert(mqtt_tree *tree, const mqtt_utf8 *topic,
                             uint32_t *node)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if ((tree != NULL) && (topic != NULL) && (node != NULL)) {
        answer = tree_insert_body(tree, topic, node);
    }
    return answer;
}

/**
 * @brief Do what mqtt_tree_find does, for arguments known to be valid.
 * @param[in] tree The tree.
 * @param[in] topic A Topic Filter or Topic Name that has been checked.
 * @return The node, or MQTT_TREE_NONE.
 */
static uint32_t tree_find_body(const mqtt_tree *tree, const mqtt_utf8 *topic)
{
    /** @brief Number of bytes in the topic. */
    const uint32_t length = (uint32_t)topic->length;
    /** @brief The node reached. */
    uint32_t current = TREE_ROOT;
    /** @brief Index where the level starts. */
    uint32_t start = 0U;

    while ((current != MQTT_TREE_NONE) && (start <= length)) {
        /** @brief Index where the level ends. */
        const uint32_t end = tree_level_end(topic->data, length, start);

        current =
            tree_step_find(tree, current, &topic->data[start], end - start);
        start = end + 1U;
    }
    return current;
}

uint32_t mqtt_tree_find(const mqtt_tree *tree, const mqtt_utf8 *topic)
{
    /** @brief The answer. */
    uint32_t answer = MQTT_TREE_NONE;

    if ((tree != NULL) && (topic != NULL)) {
        answer = tree_find_body(tree, topic);
    }
    return answer;
}

/**
 * @brief Decide whether anything still refers to a node.
 * @param[in] node The node.
 * @return true if it has a child or holds anything for the owner.
 */
static bool tree_node_held(const mqtt_tree_node *node)
{
    return (node->children != MQTT_TREE_NONE) ||
           (node->single_child != MQTT_TREE_NONE) ||
           (node->multi_child != MQTT_TREE_NONE) ||
           (node->subscriptions != MQTT_TREE_UNUSED) ||
           (node->groups != MQTT_TREE_UNUSED) ||
           (node->retained != MQTT_TREE_UNUSED);
}

/**
 * @brief Do what mqtt_tree_prune does, for arguments known to be valid.
 * @param[in,out] tree The tree.
 * @param[in] node The node to start from.
 */
static void tree_prune_body(mqtt_tree *tree, uint32_t node)
{
    /** @brief The node being looked at. */
    uint32_t current = node;
    /** @brief true while nodes can be removed. */
    bool removing = true;

    while (removing && (current != TREE_ROOT) &&
           mqtt_slab_used(&tree->node_slots, current)) {
        /** @brief The node being looked at. */
        const mqtt_tree_node *entry = &tree->nodes[current];

        if (tree_node_held(entry)) {
            removing = false;
        } else {
            /** @brief The node above. */
            const uint32_t parent = (uint32_t)entry->parent;

            tree_unlink(tree, current);
            tree_text_free(tree, entry->text_offset,
                           (uint32_t)entry->text_length);
            (void)mqtt_slab_give(&tree->node_slots, current);
            current = parent;
        }
    }
}

void mqtt_tree_prune(mqtt_tree *tree, uint32_t node)
{
    if (tree != NULL) {
        tree_prune_body(tree, node);
    }
}

/**
 * @brief Do what mqtt_tree_next does, for arguments known to be valid.
 * @param[in] tree The tree.
 * @param[in] from The node to look from.
 * @return The first node in use at or after it, or MQTT_TREE_NONE.
 */
static uint32_t tree_next_body(const mqtt_tree *tree, uint32_t from)
{
    /** @brief The slot of the node in use. */
    const uint32_t slot = mqtt_slab_next(&tree->node_slots, from);

    return (slot == MQTT_SLAB_NONE) ? MQTT_TREE_NONE : slot;
}

uint32_t mqtt_tree_next(const mqtt_tree *tree, uint32_t from)
{
    /** @brief The answer. */
    uint32_t answer = MQTT_TREE_NONE;

    if (tree != NULL) {
        answer = tree_next_body(tree, from);
    }
    return answer;
}

/**
 * @brief Find the level a node stands for.
 * @param[in] tree The tree.
 * @param[in] node A node in use, other than the root.
 * @param[out] wildcard The wildcard character, or 0 for a level of text.
 * @return Number of bytes of the level.
 */
static uint32_t tree_level_of(const mqtt_tree *tree, uint32_t node,
                              uint8_t *wildcard)
{
    /** @brief The node. */
    const mqtt_tree_node *entry = &tree->nodes[node];
    /** @brief The node above it. */
    const mqtt_tree_node *above = &tree->nodes[entry->parent];
    /** @brief Number of bytes. */
    uint32_t length = (uint32_t)entry->text_length;

    *wildcard = 0U;
    if ((uint32_t)above->single_child == node) {
        *wildcard = (uint8_t)MQTT_SINGLE_LEVEL_WILDCARD;
        length = 1U;
    } else if ((uint32_t)above->multi_child == node) {
        *wildcard = (uint8_t)MQTT_MULTI_LEVEL_WILDCARD;
        length = 1U;
    } else {
        /* A level of text, which may be empty. */
    }
    return length;
}

/**
 * @brief Do what mqtt_tree_path_length does, for arguments known to be valid.
 * @param[in] tree The tree.
 * @param[in] node A node in use, other than the root.
 * @return Number of bytes.
 */
static uint32_t tree_path_length_body(const mqtt_tree *tree, uint32_t node)
{
    /** @brief The node being looked at. */
    uint32_t current = node;
    /** @brief Number of bytes so far. */
    uint32_t length = 0U;

    while ((current != TREE_ROOT) && (current != MQTT_TREE_NONE)) {
        /** @brief The wildcard, which only the length needs here. */
        uint8_t wildcard = 0U;

        length += tree_level_of(tree, current, &wildcard);
        current = (uint32_t)tree->nodes[current].parent;
        if (current != TREE_ROOT) {
            length += 1U;
        }
    }
    return length;
}

uint32_t mqtt_tree_path_length(const mqtt_tree *tree, uint32_t node)
{
    /** @brief The answer. */
    uint32_t answer = 0U;

    if (tree != NULL) {
        answer = tree_path_length_body(tree, node);
    }
    return answer;
}

/**
 * @brief Do what mqtt_tree_path_copy does, for arguments known to be valid.
 * @param[in] tree The tree.
 * @param[in] node A node in use, other than the root.
 * @param[out] bytes Where it goes, mqtt_tree_path_length bytes of it.
 * @param[in] capacity Number of bytes there.
 * @return Number of bytes written, or 0xFFFFFFFF with nothing written if they
 * don't fit.
 */
static uint32_t tree_path_copy_body(const mqtt_tree *tree, uint32_t node,
                                    uint8_t *bytes, uint32_t capacity)
{
    /** @brief Number of bytes it takes. */
    const uint32_t length = mqtt_tree_path_length(tree, node);
    /** @brief The node being written. */
    uint32_t current = node;
    /** @brief Where the level being written ends. */
    uint32_t end = length;

    while ((length <= capacity) && (current != TREE_ROOT) &&
           (current != MQTT_TREE_NONE)) {
        /** @brief The wildcard, or 0. */
        uint8_t wildcard = 0U;
        /** @brief Number of bytes of the level. */
        const uint32_t size = tree_level_of(tree, current, &wildcard);

        end -= size;
        if (wildcard != 0U) {
            bytes[end] = wildcard;
        } else if (size > 0U) {
            mqtt_bytes_put(&bytes[end],
                           &tree->text[tree->nodes[current].text_offset], size);
        } else {
            /* An empty level writes nothing. */
        }
        current = (uint32_t)tree->nodes[current].parent;
        if (current != TREE_ROOT) {
            end -= 1U;
            bytes[end] = (uint8_t)MQTT_TOPIC_LEVEL_SEPARATOR;
        }
    }
    return (length <= capacity) ? length : 0xFFFFFFFFU;
}

uint32_t mqtt_tree_path_copy(const mqtt_tree *tree, uint32_t node,
                             uint8_t *bytes, uint32_t capacity)
{
    /** @brief The answer. */
    uint32_t answer = 0xFFFFFFFFU;

    if ((tree != NULL) && (bytes != NULL)) {
        answer = tree_path_copy_body(tree, node, bytes, capacity);
    }
    return answer;
}

/**
 * @brief Start a match or a walk at the root.
 * @param[out] match The state.
 * @param[in] topic A Topic Name or Topic Filter.
 */
static void tree_search_begin(mqtt_tree_match *match, const mqtt_utf8 *topic)
{
    match->name = topic->data;
    match->length = (uint32_t)topic->length;
    match->node = TREE_ROOT;
    match->position = 0U;
    match->pending = MQTT_TREE_NONE;
    match->anchor = MQTT_TREE_NONE;
    match->step = MQTT_TREE_STEP_ARRIVE;
    match->dollar = (topic->length > 0U) &&
                    ((uint32_t)topic->data[0] == MQTT5_TOPIC_DOLLAR);
    match->siblings = false;
    match->reach_dollar = false;
    match->only_root_wildcards = false;
}

/**
 * @brief Find the index where the level before another one starts.
 * @param[in] match The state.
 * @param[in] position Index where the later level starts, which is not the
 * first.
 * @return The index.
 */
static uint32_t tree_level_before(const mqtt_tree_match *match,
                                  uint32_t position)
{
    /** @brief The index, starting from the byte after the level before. */
    uint32_t index = position - 1U;

    while ((index > 0U) &&
           ((uint32_t)match->name[index - 1U] != MQTT_TOPIC_LEVEL_SEPARATOR)) {
        index -= 1U;
    }
    return index;
}

/**
 * @brief Find the child of the walk's node for the level that starts at its
 * position.
 * @param[in] tree The tree.
 * @param[in] match The state.
 * @param[in] node The node.
 * @param[in] end Index where the level ends.
 * @return The child, or MQTT_TREE_NONE.
 */
static uint32_t tree_literal_child(const mqtt_tree *tree,
                                   const mqtt_tree_match *match,
                                   const mqtt_tree_node *node, uint32_t end)
{
    /** @brief The child. */
    uint32_t child = MQTT_TREE_NONE;

    if (node->children != MQTT_TREE_NONE) {
        child =
            tree_trie_find(tree, match->node, &match->name[match->position],
                           end - match->position);
    }
    return child;
}

/**
 * @brief Move a match or a walk to a node below the one it is at.
 * @param[in,out] match The state.
 * @param[in] child The node.
 * @param[in] position Index where the level to match below that node starts.
 */
static void tree_search_descend(mqtt_tree_match *match, uint32_t child,
                                uint32_t position)
{
    match->node = child;
    match->position = position;
    match->step = MQTT_TREE_STEP_ARRIVE;
}

/**
 * @brief Do what mqtt_tree_match_begin does, for arguments known to be valid.
 * @param[out] match The state of the match.
 * @param[in] topic_name A Topic Name that has been checked.
 */
static void tree_match_begin_body(mqtt_tree_match *match,
                                  const mqtt_utf8 *topic_name)
{
    tree_search_begin(match, topic_name);
}

void mqtt_tree_match_begin(mqtt_tree_match *match, const mqtt_utf8 *topic_name)
{
    if ((match != NULL) && (topic_name != NULL)) {
        tree_match_begin_body(match, topic_name);
    }
}

/**
 * @brief Do what mqtt_tree_match_begin_root_wildcards does, for arguments known
 * to be valid.
 * @param[out] match The state of the match.
 * @param[in] topic_name A Topic Name that has been checked.
 */
static void tree_match_begin_root_wildcards_body(mqtt_tree_match *match,
                                                 const mqtt_utf8 *topic_name)
{
    tree_search_begin(match, topic_name);
    match->dollar = false;
    match->only_root_wildcards = true;
}

void mqtt_tree_match_begin_root_wildcards(mqtt_tree_match *match,
                                          const mqtt_utf8 *topic_name)
{
    if ((match != NULL) && (topic_name != NULL)) {
        tree_match_begin_root_wildcards_body(match, topic_name);
    }
}

/**
 * @brief Do what mqtt_tree_match_next does, for arguments known to be valid.
 * @param[in] tree The tree.
 * @param[in,out] match The state of the match.
 * @return The node, or MQTT_TREE_NONE at the end.
 */
static uint32_t tree_match_next_body(const mqtt_tree *tree,
                                     mqtt_tree_match *match)
{
    /** @brief The node handed back. */
    uint32_t result = match->pending;

    match->pending = MQTT_TREE_NONE;
    while ((result == MQTT_TREE_NONE) && (match->node != MQTT_TREE_NONE)) {
        /** @brief The node reached. */
        const mqtt_tree_node *node = &tree->nodes[match->node];
        /** @brief true if wildcards are allowed at this point. */
        const bool wild = (match->position > 0U) || (!match->dollar);

        if (match->step == MQTT_TREE_STEP_ARRIVE) {
            /* A multi-level wildcard here matches this level and all levels
             * below it. */
            if (wild) {
                result = (uint32_t)node->multi_child;
            }
            if (match->position > match->length) {
                if (result == MQTT_TREE_NONE) {
                    result = match->node;
                } else {
                    match->pending = match->node;
                }
                match->step = MQTT_TREE_STEP_LEAVE;
            } else {
                /** @brief Index where the level ends. */
                const uint32_t end =
                    tree_level_end(match->name, match->length, match->position);
                /** @brief The child for this level's text. */
                const uint32_t child =
                    (match->only_root_wildcards && (match->position == 0U)) ?
                        MQTT_TREE_NONE :
                        tree_literal_child(tree, match, node, end);

                match->step = MQTT_TREE_STEP_AFTER_EXACT;
                if (child != MQTT_TREE_NONE) {
                    tree_search_descend(match, child, end + 1U);
                }
            }
        } else if (match->step == MQTT_TREE_STEP_AFTER_EXACT) {
            match->step = MQTT_TREE_STEP_LEAVE;
            if (wild && (node->single_child != MQTT_TREE_NONE)) {
                tree_search_descend(match, (uint32_t)node->single_child,
                                    tree_level_end(match->name, match->length,
                                                   match->position) +
                                        1U);
            }
        } else if (match->node == TREE_ROOT) {
            match->node = MQTT_TREE_NONE;
        } else {
            /** @brief The node above. */
            const uint32_t parent = (uint32_t)node->parent;

            /* The child for the text comes before the one for the wildcard, so
             * that one is next. */
            match->step =
                ((uint32_t)tree->nodes[parent].single_child == match->node) ?
                    MQTT_TREE_STEP_LEAVE : MQTT_TREE_STEP_AFTER_EXACT;
            match->position = tree_level_before(match, match->position);
            match->node = parent;
        }
    }
    return result;
}

uint32_t mqtt_tree_match_next(const mqtt_tree *tree, mqtt_tree_match *match)
{
    /** @brief The answer. */
    uint32_t answer = MQTT_TREE_NONE;

    if ((tree != NULL) && (match != NULL)) {
        answer = tree_match_next_body(tree, match);
    }
    return answer;
}

/**
 * @brief Do what mqtt_tree_walk_begin does, for arguments known to be valid.
 * @param[out] cursor The state of the walk.
 * @param[in] topic_filter A Topic Filter that has been checked.
 */
static void tree_walk_begin_body(mqtt_tree_match *cursor,
                                 const mqtt_utf8 *topic_filter)
{
    tree_search_begin(cursor, topic_filter);
}

void mqtt_tree_walk_begin(mqtt_tree_match *cursor,
                          const mqtt_utf8 *topic_filter)
{
    if ((cursor != NULL) && (topic_filter != NULL)) {
        tree_walk_begin_body(cursor, topic_filter);
    }
}

void mqtt_tree_walk_reach_dollar(mqtt_tree_match *cursor)
{
    if (cursor != NULL) {
        cursor->reach_dollar = true;
    }
}

/**
 * @brief Decide whether a node's text starts with a dollar.
 * @param[in] tree The tree.
 * @param[in] node The node.
 * @return true if it does.
 */
static bool tree_node_is_dollar(const mqtt_tree *tree,
                                const mqtt_tree_node *node)
{
    return (node->text_length > 0U) &&
           ((uint32_t)tree->text[node->text_offset] == MQTT5_TOPIC_DOLLAR);
}

/**
 * @brief Do what a walk does on arriving at a node.
 * @param[in] tree The tree.
 * @param[in,out] cursor The state.
 * @return The node, if the walk reports it, or MQTT_TREE_NONE.
 */
static uint32_t tree_walk_arrive(const mqtt_tree *tree, mqtt_tree_match *cursor)
{
    /** @brief The node reached. */
    const uint32_t reached = cursor->node;
    /** @brief The node's contents. */
    const mqtt_tree_node *node = &tree->nodes[reached];
    /** @brief The node handed back. */
    uint32_t result = MQTT_TREE_NONE;

    cursor->step = MQTT_TREE_STEP_LEAVE;
    /* A wildcard at the root doesn't reach a topic that starts with a
     * dollar. */
    if (cursor->siblings && (!cursor->reach_dollar) &&
        (node->parent == TREE_ROOT) && tree_node_is_dollar(tree, node)) {
        /* Nothing here or below it counts. */
    } else if (cursor->position > cursor->length) {
        result = reached;
    } else {
        /** @brief Index where the filter's level ends. */
        const uint32_t end =
            tree_level_end(cursor->name, cursor->length, cursor->position);
        /** @brief The filter's level. */
        const uint8_t *text = &cursor->name[cursor->position];
        /** @brief true if the level is a single character. */
        const bool single = ((end - cursor->position) == 1U);
        /** @brief The first of the node's children whose levels are text. */
        const uint32_t first = tree_first_child(tree, reached);

        if (single && ((uint32_t)text[0] == MQTT_MULTI_LEVEL_WILDCARD)) {
            /* This node and everything below it. */
            result = reached;
            if (cursor->anchor == MQTT_TREE_NONE) {
                cursor->anchor = reached;
            }
            if (first != MQTT_TREE_NONE) {
                cursor->siblings = true;
                tree_search_descend(cursor, first, cursor->position);
            }
        } else if (single &&
                   ((uint32_t)text[0] == MQTT_SINGLE_LEVEL_WILDCARD)) {
            if (first != MQTT_TREE_NONE) {
                cursor->siblings = true;
                tree_search_descend(cursor, first, end + 1U);
            }
        } else {
            /** @brief The child for this level's text. */
            const uint32_t child = tree_literal_child(tree, cursor, node, end);

            if (child != MQTT_TREE_NONE) {
                cursor->siblings = false;
                tree_search_descend(cursor, child, end + 1U);
            }
        }
    }
    return result;
}

/**
 * @brief Do what a walk does on finishing with a node and everything below it.
 * @param[in] tree The tree.
 * @param[in,out] cursor The state.
 */
static void tree_walk_leave(const mqtt_tree *tree, mqtt_tree_match *cursor)
{
    /** @brief The node's contents. */
    const mqtt_tree_node *node = &tree->nodes[cursor->node];

    if (cursor->anchor == cursor->node) {
        cursor->anchor = MQTT_TREE_NONE;
    }
    if (cursor->node == TREE_ROOT) {
        cursor->node = MQTT_TREE_NONE;
    } else if (cursor->siblings &&
               ((uint32_t)node->next !=
                tree_first_child(tree, (uint32_t)node->parent))) {
        cursor->node = (uint32_t)node->next;
        cursor->step = MQTT_TREE_STEP_ARRIVE;
    } else {
        /* Below a multi-level wildcard, every node is at the same level of the
         * filter. */
        if (cursor->anchor == MQTT_TREE_NONE) {
            cursor->position = tree_level_before(cursor, cursor->position);
        }
        cursor->node = (uint32_t)node->parent;
        if ((cursor->anchor != MQTT_TREE_NONE) &&
            (cursor->anchor != cursor->node)) {
            cursor->siblings = true;
        } else if (cursor->position == 0U) {
            cursor->siblings = false;
        } else {
            /** @brief Index where the level that led to this node starts. */
            const uint32_t start = tree_level_before(cursor, cursor->position);

            cursor->siblings =
                ((cursor->position - start) == 2U) &&
                ((uint32_t)cursor->name[start] == MQTT_SINGLE_LEVEL_WILDCARD);
        }
    }
}

/**
 * @brief Do what mqtt_tree_walk_next does, for arguments known to be valid.
 * @param[in] tree The tree.
 * @param[in,out] cursor The walk's state, from mqtt_tree_walk_begin.
 * @return The node, or MQTT_TREE_NONE at the end.
 */
static uint32_t tree_walk_next_body(const mqtt_tree *tree,
                                    mqtt_tree_match *cursor)
{
    /** @brief The node handed back. */
    uint32_t result = MQTT_TREE_NONE;

    while ((result == MQTT_TREE_NONE) && (cursor->node != MQTT_TREE_NONE)) {
        if (cursor->step == MQTT_TREE_STEP_ARRIVE) {
            result = tree_walk_arrive(tree, cursor);
        } else {
            tree_walk_leave(tree, cursor);
        }
    }
    return result;
}

uint32_t mqtt_tree_walk_next(const mqtt_tree *tree, mqtt_tree_match *cursor)
{
    /** @brief The answer. */
    uint32_t answer = MQTT_TREE_NONE;

    if ((tree != NULL) && (cursor != NULL)) {
        answer = tree_walk_next_body(tree, cursor);
    }
    return answer;
}
