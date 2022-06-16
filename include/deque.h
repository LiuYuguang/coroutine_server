
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _DEQUE_H_
#define _DEQUE_H_
// 双向队列
#include <stddef.h>

typedef struct deque_s  deque_t;

struct deque_s {
    deque_t  *prev;
    deque_t  *next;
};


#define deque_init(q)                                                     \
    (q)->prev = q;                                                            \
    (q)->next = q


#define deque_empty(h)                                                    \
    (h == (h)->prev)


#define deque_insert_head(h, x)                                           \
    (x)->next = (h)->next;                                                    \
    (x)->next->prev = x;                                                      \
    (x)->prev = h;                                                            \
    (h)->next = x


#define deque_insert_after   deque_insert_head


#define deque_insert_tail(h, x)                                           \
    (x)->prev = (h)->prev;                                                    \
    (x)->prev->next = x;                                                      \
    (x)->next = h;                                                            \
    (h)->prev = x
#define deque_push deque_insert_tail


#define deque_head(h)                                                     \
    (h)->next


#define deque_last(h)                                                     \
    (h)->prev


#define deque_sentinel(h)                                                 \
    (h)


#define deque_next(q)                                                     \
    (q)->next


#define deque_prev(q)                                                     \
    (q)->prev

#define deque_remove(x)                                                   \
    (x)->next->prev = (x)->prev;                                              \
    (x)->prev->next = (x)->next;                                              \
    (x)->prev = x;                                                         \
    (x)->next = x

#define deque_pop(h) \
    ({typeof((h)->next) _deque_node = (h)->next;\
    deque_remove(_deque_node);\
    _deque_node;\
    })

#define deque_extend(h1, h2) \
    (h1)->prev->next = (h2)->next;\
    (h2)->next->prev = (h1)->prev;\
    (h1)->prev = (h2);\
    (h2)->next = (h1)


#define deque_data(q, type, link)                                         \
    (type *) ((u_char *) q - offsetof(type, link))

#endif /* _NGX_deque_H_INCLUDED_ */
