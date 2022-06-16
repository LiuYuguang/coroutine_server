#ifndef _COROUTINE_H_
#define _COROUTINE_H_

#include <stdint.h>

typedef struct schedule_s schedule;
typedef void (*coroutine_func)(schedule*,void*);

schedule * coroutine_open(void);
void coroutine_close(schedule *S);
uint64_t coroutine_new(schedule *S, coroutine_func func, void *arg);
int coroutine_loop(schedule *S);

uint64_t coroutine_id(schedule *S);// 获取当前id
int coroutine_yield(schedule *S);// 单纯让出协程
int coroutine_wait(schedule *S); // 等待某件事件发生,可以是fd,可以是timeout,可以是wake
int coroutine_io_ctl(schedule *S, int fd, int opt, unsigned int events);// 设置fd事件发生, 其实就是对应epoll_ctl
int coroutine_alarm(schedule *S, unsigned int seconds);// 设置超时中断, 1ms
int coroutine_wake(schedule *S,uint64_t id);// 唤醒对应协程id
int coroutine_cond_acquire(schedule *S);// 获取锁
int coroutine_cond_release(schedule *S);// 释放锁
int coroutine_cond_wait(schedule *S);// 等待条件变量
int coroutine_cond_notify(schedule *S);// 通知一个已满足条件变量
int coroutine_cond_notify_all(schedule *S);// 通知所有已满足条件变量

#endif