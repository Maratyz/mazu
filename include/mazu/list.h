/* SPDX-License-Identifier: MIT */
/* Linux-style circular doubly-linked list.
 *
 * A sentinel (head) node whose prev/next always point to valid nodes
 * (or back to itself when the list is empty) eliminates all NULL checks
 * and special cases.  Every list_del() makes the removed node
 * self-referential, so a double-remove is a harmless no-op.
 *
 * In debug builds (__DEBUG__ > 0), list_del() poisons the removed node's
 * pointers so that a subsequent dereference crashes immediately instead
 * of silently operating on a stale linkage.  list_del_init() preserves
 * the original self-referential behavior for callers that need safe
 * double-remove (e.g. teardown paths that may unlink twice).
 */

#ifndef MAZU_LIST_H
#define MAZU_LIST_H

#include <mazu/assert.h>

/* Poison values for freed list nodes - deliberately misaligned so any
 * dereference on RISC-V triggers an alignment fault immediately.
 */
#define LIST_POISON_PREV ((struct list_head *) 0xDEAD000000000010UL)
#define LIST_POISON_NEXT ((struct list_head *) 0xDEAD000000000028UL)

enum list_node_state {
    LIST_NODE_INIT = 0,
    LIST_NODE_LINKED = 1,
    LIST_NODE_DETACHED = 2,
};

struct list_head {
    struct list_head *prev;
    struct list_head *next;
#if __DEBUG__ > 0
    enum list_node_state state;
#endif
};

/* Static initializer for a list head. */
#if __DEBUG__ > 0
#define LIST_HEAD_INIT(name) {&(name), &(name), LIST_NODE_INIT}
#else
#define LIST_HEAD_INIT(name) {&(name), &(name)}
#endif

/* Declare and initialize a list head variable. */
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

/* Runtime-initialize a list head as an empty self-loop. */
#define INIT_LIST_HEAD(ptr) list_init(ptr)

/* Initialize a list head as an empty self-loop. */
static inline void list_init(struct list_head *head)
{
    head->next = head;
    head->prev = head;
#if __DEBUG__ > 0
    head->state = LIST_NODE_INIT;
#endif
}

/* Insert @node immediately after @head. */
static inline void list_add(struct list_head *head, struct list_head *node)
{
#if __DEBUG__ > 0
    DEBUG_ASSERT(node->state != LIST_NODE_LINKED);
#endif
    node->next = head->next;
    node->prev = head;
    head->next->prev = node;
    head->next = node;
#if __DEBUG__ > 0
    node->state = LIST_NODE_LINKED;
#endif
}

/* Insert @node immediately before @head (i.e. at the tail). */
static inline void list_add_tail(struct list_head *head, struct list_head *node)
{
#if __DEBUG__ > 0
    DEBUG_ASSERT(node->state != LIST_NODE_LINKED);
#endif
    node->next = head;
    node->prev = head->prev;
    head->prev->next = node;
    head->prev = node;
#if __DEBUG__ > 0
    node->state = LIST_NODE_LINKED;
#endif
}

/* Unlink @node from its list and reinitialize as self-referential.
 * Safe to call on an already-removed (self-referential) node - no-op.
 * Use this in teardown paths where double-remove must be tolerated.
 */
static inline void list_del_init(struct list_head *node)
{
    node->prev->next = node->next;
    node->next->prev = node->prev;
    list_init(node);
}

/* Unlink @node from its list.
 * In debug builds, poisons the removed node so that any subsequent
 * dereference crashes immediately (use-after-remove detection).
 * In release builds, makes the node self-referential (double-remove safe).
 */
static inline void list_del(struct list_head *node)
{
#if __DEBUG__ > 0
    DEBUG_ASSERT(node->state == LIST_NODE_LINKED);
#endif
    node->prev->next = node->next;
    node->next->prev = node->prev;
#if __DEBUG__ > 0
    node->prev = LIST_POISON_PREV;
    node->next = LIST_POISON_NEXT;
    node->state = LIST_NODE_DETACHED;
#else
    list_init(node);
#endif
}

/* Return true if the list headed by @head contains no entries. */
static inline bool list_empty(struct list_head *head)
{
    return head->next == head;
}

/* Get the struct that contains this list entry. */
#define list_entry(ptr, type, member) __container_of(ptr, type, member)

/* Safe iteration: pre-load the next link so removal of the current entry
 * within the loop body is safe. The iterator variable must be declared by
 * the caller before invoking this macro.
 */
#define list_for_each_entry_safe(_list, _iter, _type, _member)           \
    for (struct list_head *_iter##_node = (_list)->next,                 \
                          *_iter##_next = _iter##_node->next;            \
         _iter##_node != (_list) &&                                      \
         ((_iter) = __container_of(_iter##_node, _type, _member), true); \
         _iter##_node = _iter##_next, _iter##_next = _iter##_node->next)

#endif /* MAZU_LIST_H */
