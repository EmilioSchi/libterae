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
 * @file base_tree.h
 * @brief The tree of topic levels that holds subscriptions and retained
 * messages.
 *
 * Each node is one level of a Topic Filter or Topic Name. The children of a
 * node whose levels are text are the leaves of a crit-bit trie: each branch of
 * it tests one bit of the level, the first bit at which the children below it
 * differ, so a child is found in as many steps as its level has bits at most,
 * however many siblings it has, and no choice of levels makes that longer. A
 * branch always has both of its children, and k children take k - 1 branches,
 * from a pool of branches beside the nodes. The same children also form a
 * circle, in the order they were added, which is how a walk goes through them.
 * Wildcard children have a link of their own each. Links are 16-bit indexes,
 * so a tree has at most 65,535 nodes. Nodes come from a slab of pages of nodes,
 * and never move. The text of levels is packed into pages, each level whole in
 * one page after the index of its node, and a page closes up when a level in it
 * goes, telling only the nodes of that page. A level longer than a page has a
 * run of pages to itself.
 *
 * Nothing a match or a walk does changes the tree, and the tree must not change
 * while one is under way.
 *
 * Matching can't recurse, as MISRA C:2012 Rule 17.2 rules that out, and it
 * hands back one node at a time. It needs no stack either, as each node links
 * to its parent, so a topic can have as many levels as its length allows, which
 * is what the specifications ask for.
 *
 * References: E. Fredkin, Trie Memory, Communications of the ACM 3(9), 1960. D.
 * R. Morrison, PATRICIA: Practical Algorithm To Retrieve Information Coded in
 * Alphanumeric, Journal of the ACM 15(4), 1968. D. Knuth, The Art of Computer
 * Programming volume 3, sections 6.1 and 6.3. D. J. Bernstein, crit-bit trees,
 * cr.yp.to/critbit.html.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 4.7, p.102-105.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */
#ifndef MQTT_BASE_TREE_H
#define MQTT_BASE_TREE_H

#include <stdbool.h>
#include <stdint.h>

#include "base_page.h"
#include "base_slab.h"
#include "base_status.h"
#include "base_utf8_t.h"

/** @brief The index that stands for no node. */
#define MQTT_TREE_NONE 0xFFFFU
/** @brief The most nodes a tree can have, as MQTT_TREE_NONE is not an index. */
#define MQTT_TREE_NODES_MAX 0xFFFFU
/** @brief The value of a field kept for the owner that holds nothing. */
#define MQTT_TREE_UNUSED 0xFFFFFFFFU
/** @brief Number of bytes in a node. */
#define MQTT_TREE_NODE_SIZE 32U
/** @brief Number of nodes in a page. */
#define MQTT_TREE_PAGE_NODES (MQTT_PAGE_SIZE / MQTT_TREE_NODE_SIZE)
/**
 * @brief Number of bytes before each level's text in the pages of text: the
 * index of its node, high byte first. A level of text takes this many more
 * bytes than its length, and a level longer than a page less this many has a
 * run of pages to itself.
 */
#define MQTT_TREE_TEXT_HEAD 2U
/** @brief Number of bytes in a branch. */
#define MQTT_TREE_BRANCH_SIZE 10U

/**
 * @brief A branch of the trie of a node's children: it sends a level one way
 * or the other by one bit of it.
 *
 * A level is read a byte at a time, each byte with a ninth bit set, which says
 * the level has that byte, and as zero past its end. So no level reads like the
 * start of a longer one, whatever bytes it holds.
 */
typedef struct {
    /** @brief Index of the byte tested. */
    uint16_t byte;
    /** @brief The bit tested, from 0 to 7, or 8 for whether there is a byte. */
    uint8_t bit;
    /** @brief Bit d set if child[d] is a node, and clear if it is a branch. */
    uint8_t kinds;
    /**
     * @brief The two sides: child[0] for the bit clear, child[1] for it set. A
     * branch not in use keeps the next one not in use in child[0].
     */
    uint16_t child[2];
    /**
     * @brief A node below the branch. At the top of a trie, the first of the
     * circle of children.
     */
    uint16_t sample;
} mqtt_tree_branch;

/** @brief One level in the tree. */
typedef struct {
    /**
     * @brief First non-shared subscription here, or MQTT_TREE_UNUSED. Used by
     * whoever owns the tree.
     */
    uint32_t subscriptions;
    /**
     * @brief First shared group here, or MQTT_TREE_UNUSED. Used by whoever
     * owns the tree.
     */
    uint32_t groups;
    /**
     * @brief The retained message here, or MQTT_TREE_UNUSED. Used by whoever
     * owns the tree.
     */
    uint32_t retained;
    /** @brief Where the level's text starts in the tree's text, if any. */
    uint32_t text_offset;
    /** @brief Number of bytes in the level's text. */
    uint16_t text_length;
    /** @brief The node above, or MQTT_TREE_NONE at the root. */
    uint16_t parent;
    /**
     * @brief The children whose levels are text: the only one, or the branch
     * at the top of their trie, or MQTT_TREE_NONE.
     */
    uint16_t children;
    /**
     * @brief The next child of the same parent whose level is text, round in a
     * circle, or MQTT_TREE_NONE for the root and a wildcard.
     */
    uint16_t next;
    /** @brief The child before it in the same circle. */
    uint16_t previous;
    /** @brief The child for the single-level wildcard. */
    uint16_t single_child;
    /** @brief The child for the multi-level wildcard. */
    uint16_t multi_child;
    /** @brief Bit 0 set if children is a node, and clear if it is a branch. */
    uint8_t kinds;
} mqtt_tree_node;

/** @brief The memory a tree uses. */
typedef struct {
    /** @brief The nodes. */
    mqtt_tree_node *nodes;
    /** @brief Number of nodes, from 1 to MQTT_TREE_NODES_MAX. */
    uint32_t node_count;
    /**
     * @brief MQTT_PAGE_META_WORDS words for each page of nodes, of which there
     * are node_count over MQTT_TREE_PAGE_NODES, rounded up.
     */
    uint32_t *node_meta;
    /** @brief The pages of text: text_pages times MQTT_PAGE_SIZE bytes. */
    uint8_t *text;
    /** @brief Number of pages of text. */
    uint32_t text_pages;
    /** @brief MQTT_PAGE_META_WORDS words for each page of text. */
    uint32_t *text_meta;
    /** @brief The branches, or NULL if there are none. */
    mqtt_tree_branch *branches;
    /**
     * @brief Number of branches, at most MQTT_TREE_NODES_MAX and at least
     * node_count - 2, which is the most a tree of node_count nodes can need.
     */
    uint32_t branch_count;
} mqtt_tree_storage;

/** @brief A tree. */
typedef struct {
    /** @brief The nodes. */
    mqtt_tree_node *nodes;
    /** @brief Which nodes are in use. */
    mqtt_slab node_slots;
    /** @brief The pages of text. */
    uint8_t *text;
    /** @brief The pages of text in use, and the runs of them. */
    mqtt_runs text_pages;
    /** @brief The page new text goes to first, or MQTT_PAGE_NONE. */
    uint32_t text_page;
    /** @brief Number of bytes of text in use, over every page. */
    uint32_t text_used;
    /** @brief The branches. */
    mqtt_tree_branch *branches;
    /** @brief Number of branches. */
    uint32_t branch_count;
    /** @brief The first branch given back and not taken again, or none. */
    uint32_t branch_free;
    /** @brief Number of branches ever taken, which are in use or given back. */
    uint32_t branch_high;
} mqtt_tree;

/** @brief What a match or a walk does next at the node it has reached. */
typedef enum {
    /** @brief It has just arrived. */
    MQTT_TREE_STEP_ARRIVE,
    /** @brief It has finished with the child for the level's text. */
    MQTT_TREE_STEP_AFTER_EXACT,
    /** @brief It has finished with everything below, and goes on elsewhere. */
    MQTT_TREE_STEP_LEAVE
} mqtt_tree_step;

/**
 * @brief The state of a match or a walk in progress. It is small, and lives on
 * the caller's stack.
 *
 * The way back from a node is in the tree, through its parent, and where that
 * puts the search in the topic can be read from the topic itself, so this takes
 * no memory per level.
 */
typedef struct {
    /** @brief The Topic Name or Topic Filter. */
    const uint8_t *name;
    /** @brief Number of bytes in the name. */
    uint32_t length;
    /** @brief The node reached, or MQTT_TREE_NONE at the end. */
    uint32_t node;
    /**
     * @brief Index where the level to match below the node starts, past the end
     * if none left.
     */
    uint32_t position;
    /**
     * @brief A second result found while the first was handed back, or
     * MQTT_TREE_NONE.
     */
    uint32_t pending;
    /**
     * @brief In a walk, the node a multi-level wildcard began at, or
     * MQTT_TREE_NONE.
     */
    uint32_t anchor;
    /** @brief What to do next at the node. */
    mqtt_tree_step step;
    /**
     * @brief true if the name starts with a dollar, which wildcards at the root
     * must not match.
     */
    bool dollar;
    /**
     * @brief In a walk, true if the node's siblings are to be visited after it.
     */
    bool siblings;
    /**
     * @brief true if wildcards at the root reach a topic that starts with a
     * dollar.
     */
    bool reach_dollar;
    /** @brief true if a match only follows the wildcards at the root. */
    bool only_root_wildcards;
} mqtt_tree_match;

/**
 * @brief Set up a tree. Node 0 is the root and is in use from the start.
 * @param[out] tree The tree.
 * @param[in] storage Its memory.
 * @return MQTT_OK or MQTT_INVALID_ARGUMENT.
 */
mqtt_status mqtt_tree_init(mqtt_tree *tree, const mqtt_tree_storage *storage);

/**
 * @brief Find the node of a Topic Filter or Topic Name, adding any level that
 * is missing.
 * @param[in,out] tree The tree.
 * @param[in] topic A Topic Filter or Topic Name that has been checked.
 * @param[out] node The node.
 * @return MQTT_OK, MQTT_QUOTA_EXCEEDED, or MQTT_INVALID_ARGUMENT for a NULL
 * pointer, in which case nothing has been added.
 */
mqtt_status mqtt_tree_insert(mqtt_tree *tree, const mqtt_utf8 *topic,
                             uint32_t *node);

/**
 * @brief Find the node of a Topic Filter or Topic Name.
 * @param[in] tree The tree.
 * @param[in] topic A Topic Filter or Topic Name that has been checked.
 * @return The node, or MQTT_TREE_NONE.
 */
uint32_t mqtt_tree_find(const mqtt_tree *tree, const mqtt_utf8 *topic);

/**
 * @brief Remove a node, and any node above it, once nothing refers to them any
 * more. No other node moves.
 * @param[in,out] tree The tree.
 * @param[in] node The node to start from.
 */
void mqtt_tree_prune(mqtt_tree *tree, uint32_t node);

/**
 * @brief Find the next node in use, to go through all of them.
 * @param[in] tree The tree.
 * @param[in] from The node to look from.
 * @return The first node in use at or after it, or MQTT_TREE_NONE.
 */
uint32_t mqtt_tree_next(const mqtt_tree *tree, uint32_t from);

/**
 * @brief Find the length of the Topic Name or Topic Filter a node stands for:
 * its levels from the root, with the separator between them.
 * @param[in] tree The tree.
 * @param[in] node A node in use, other than the root.
 * @return Number of bytes.
 */
uint32_t mqtt_tree_path_length(const mqtt_tree *tree, uint32_t node);

/**
 * @brief Write the Topic Name or Topic Filter a node stands for.
 * @param[in] tree The tree.
 * @param[in] node A node in use, other than the root.
 * @param[out] bytes Where it goes, mqtt_tree_path_length bytes of it.
 * @param[in] capacity Number of bytes there.
 * @return Number of bytes written, or 0xFFFFFFFF with nothing written if they
 * don't fit or a pointer is NULL.
 */
uint32_t mqtt_tree_path_copy(const mqtt_tree *tree, uint32_t node,
                             uint8_t *bytes, uint32_t capacity);

/**
 * @brief Start looking for the nodes that match a Topic Name. The tree must not
 * change until the match is over.
 * @param[out] match The state of the match.
 * @param[in] topic_name A Topic Name that has been checked.
 */
void mqtt_tree_match_begin(mqtt_tree_match *match,
                           const mqtt_utf8 *topic_name);

/**
 * @brief Start looking for the nodes reached only through a wildcard at the
 * root, for a topic those wildcards don't otherwise reach as it starts with a
 * dollar. MQTT 3.1 gives a dollar no meaning.
 * @param[out] match The state of the match.
 * @param[in] topic_name A Topic Name that has been checked.
 */
void mqtt_tree_match_begin_root_wildcards(mqtt_tree_match *match,
                                          const mqtt_utf8 *topic_name);

/**
 * @brief Find the next node whose Topic Filter matches the Topic Name.
 * @param[in] tree The tree.
 * @param[in,out] match The state of the match.
 * @return The node, or MQTT_TREE_NONE at the end.
 */
uint32_t mqtt_tree_match_next(const mqtt_tree *tree, mqtt_tree_match *match);

/**
 * @brief Find the next node that holds a Topic Name, in the part of the tree a
 * filter selects.
 * @param[in] tree The tree.
 * @param[in,out] cursor The walk's state, from mqtt_tree_walk_begin.
 * @return The node, or MQTT_TREE_NONE at the end.
 */
uint32_t mqtt_tree_walk_next(const mqtt_tree *tree, mqtt_tree_match *cursor);

/**
 * @brief Start looking for the Topic Names a Topic Filter selects.
 * @param[out] cursor The state of the walk.
 * @param[in] topic_filter A Topic Filter that has been checked.
 */
void mqtt_tree_walk_begin(mqtt_tree_match *cursor,
                          const mqtt_utf8 *topic_filter);

/**
 * @brief Say that a walk just begun should reach topics that start with a
 * dollar from a wildcard at the root, as MQTT 3.1 gives a dollar no meaning.
 * @param[in,out] cursor The state of the walk.
 */
void mqtt_tree_walk_reach_dollar(mqtt_tree_match *cursor);

#endif /* MQTT_BASE_TREE_H */
