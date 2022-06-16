#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include "timer.h"
#include "deque.h"

inline void link_clear(struct timer_node *list,struct timer_node *head){
    deque_extend(list,head);
    deque_remove(list);
}

inline void link(struct timer_node *list,struct timer_node *node){
    deque_insert_tail(list,node);
}

static uint64_t
gettime() {
	uint64_t t;
	struct timespec ti;
	clock_gettime(CLOCK_MONOTONIC, &ti);
	t = (uint64_t)ti.tv_sec * 100;
	t += ti.tv_nsec / 10000000;
	return t;
}

void add_node(struct timer *T,struct timer_node *node){
    uint32_t time = node->expire;
    uint32_t current_time = T->time;
    if((time|TIME_NEAR_MASK) == (current_time|TIME_NEAR_MASK)){
        link(&T->near[time&TIME_NEAR_MASK],node);
    }else{
        int i;
        uint32_t mask = TIME_NEAR << TIME_LEVEL_SHIFT;
        for(i=0;i<3;i++){
            if((time|(mask-1)) == (current_time|(mask-1))){
                break;
            }
            mask <<= TIME_LEVEL_SHIFT;
        }
        link(&T->t[i][((time>>(TIME_NEAR_SHIFT + i*TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK)],node);
    }
}

void timer_add(struct timer *T,struct timer_node *node,unsigned int time){
    node->expire = time/10 + T->time;
    add_node(T,node);
}

void move_list(struct timer *T,int level,int idx){
    struct timer_node head,*node;
    deque_init(&head);
    link_clear(&T->t[level][idx],&head);
    while(!deque_empty(&head)){
        node = deque_head(&head);
        deque_remove(node);
        add_node(T,node);
    }
}

void timer_shift(struct timer *T){
    int mask = TIME_NEAR;
    uint32_t ct = ++T->time;
    if(ct == 0){// 溢出
        move_list(T,3,0);
    }else{
        uint32_t time = ct >> TIME_NEAR_SHIFT;
        int i = 0;

        while((ct & (mask - 1)) == 0){// ct在TIME_NEAR的倍数下才会进入
            int idx = time & TIME_LEVEL_MASK;
            if(idx!=0){
                move_list(T,i,idx);
                break;
            }
            mask <<= TIME_LEVEL_SHIFT;
            time >>= TIME_LEVEL_SHIFT;
            ++i;
        }
    }
}

void timer_execute(struct timer* T,struct timer_node *head){
    int idx = T->time & TIME_NEAR_MASK;

    while(!deque_empty(&T->near[idx])){
        link_clear(&T->near[idx],head);
    }
}

uint32_t timer_near(struct timer* T){
    int idx;
    for(idx = T->time & TIME_NEAR_MASK;idx<TIME_NEAR && deque_empty(&T->near[idx]);idx++){}
    return (idx-(T->time & TIME_NEAR_MASK))*10;
}

void timer_update(struct timer *T,struct timer_node *head){
    // try to dispatch timeout 0 (rare condition)
    timer_execute(T,head);

    // shift time first, and then dispatch timer message
    timer_shift(T);

    timer_execute(T,head);
}

void skynet_updatetime(struct timer *TI,struct timer_node *head){
    uint64_t cp = gettime();
    if(cp < TI->current_point){
        // error
        TI->current_point = cp;
    }else if(cp != TI->current_point){
        uint32_t diff = (uint32_t)(cp - TI->current_point);
        // printf("diff %u\n",diff);
        TI->current_point = cp;
        // TI->current += diff;
        int i;
        for(i=0;i<diff;i++){
            timer_update(TI,head);
        }
    }
}

void skynet_timer_init(struct timer *TI){
    int i,j;
    for(i=0;i<TIME_NEAR;i++){
        deque_init(&TI->near[i]);
    }

    for(i=0;i<4;i++){
        for(j=0;j<TIME_LEVEL;j++){
            deque_init(&TI->t[i][j]);
        }
    }
    TI->time = 0;
    TI->current_point = gettime();
}
