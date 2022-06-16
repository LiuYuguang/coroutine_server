#ifndef _QUEUE_H_
#define _QUEUE_H_
// 单向队列

#include <stddef.h>

typedef struct queue_node_s  queue_node_t;
typedef struct queue_head_s  queue_head_t;
struct queue_node_s {
    queue_node_t  *next;
};

struct queue_head_s {
    queue_node_t  head;
    queue_node_t  *tail;
};

#define queue_init(h) \
    (h)->head.next = NULL;   \
    (h)->tail = &((h)->head)

#define queue_empty(h)  \
    (NULL == (h)->head.next)

#define queue_push(h, x)  \
    (h)->tail->next = (x);    \
    (h)->tail = (x);         \
    (x)->next = NULL

#define queue_first(h) \
    ((h)->head.next)

#define queue_pop(h) \
    ({ \
        typeof((h)->head.next) _x = (h)->head.next; \
        (h)->head.next = _x->next;\
        if((h)->head.next == NULL) (h)->tail = &((h)->head); \
        _x; \
    })

#define queue_data(q, type, link)                                         \
    (type *) ((u_char *) q - offsetof(type, link))

#endif /* _NGX_QUEUE_H_INCLUDED_ */
