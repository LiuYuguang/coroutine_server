#ifndef _TIMER_H_
#define _TIMER_H_

#include <stdint.h>

#define TIME_NEAR_SHIFT 8
#define TIME_NEAR (1 << TIME_NEAR_SHIFT)
#define TIME_LEVEL_SHIFT 6
#define TIME_LEVEL (1 << TIME_LEVEL_SHIFT)
#define TIME_NEAR_MASK (TIME_NEAR-1)
#define TIME_LEVEL_MASK (TIME_LEVEL-1)

struct timer_node{
    struct timer_node *next,*prev;
    uint32_t expire;
};

struct timer{
    struct timer_node near[TIME_NEAR];
    struct timer_node t[4][TIME_LEVEL];
    uint32_t time;// timer启动的时间10ms
    uint64_t current_point; // 自系统启动时间ms
};

void timer_add(struct timer *T,struct timer_node *node,unsigned int time);// time ms
void skynet_updatetime(struct timer *TI,struct timer_node *head);
void skynet_timer_init(struct timer *TI);
uint32_t timer_near(struct timer* T);// time ms

#endif