#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <ucontext.h>
#include "coroutine.h"
#include <stdio.h>
#include <unistd.h>
#include "queue.h"
#include "deque.h"
#include "timer.h"
#include <errno.h>
#include "btree.h"

#define STACK_SIZE (1024*1024)

#define COROUTINE_DEAD 0
#define COROUTINE_INIT 1
#define COROUTINE_READY 2
#define COROUTINE_BLOCKED 3
#define COROUTINE_RUNNING 4

#define MAXEVENT 100

#define save_stack() \
    register void *sp asm("rsp"); \
    ptrdiff_t size = S->stack + STACK_SIZE - (char*)sp; \
    if(co->cap < size){\
        free(co->stack);\
        co->cap = size;\
        co->stack = malloc(co->cap);\
    }\
    co->len = size;\
    memcpy(co->stack, S->stack + STACK_SIZE - size, size);


typedef struct{
    uint64_t id;
	coroutine_func func;
	void *arg;
    int status;
    int fd;
    int rc;
    int co_errno;
	ucontext_t ctx;
    deque_t lock_node;// 锁队列, 双向队列
    queue_node_t ready_node;// 就绪队列, 单向队列
    struct timer_node timer_node;// 双向队列
    size_t len,cap;
	char *stack;
}coroutine;

typedef struct schedule_s{
    char stack[STACK_SIZE];
	ucontext_t main;
    uint64_t nco_id;
    uint64_t nco_alive;
    uint64_t blocked;
    deque_t lock;  // 锁队列, 双向队列
    deque_t cond;  // 条件队列, 双向队列
	queue_head_t ready; // 就绪队列, 单向队列
    coroutine *running; // 运行队列, 只能有一个在运行
    struct timer TI;// 双向队列
    int epfd;
    intptr_t tree[0];
}schedule;

schedule * coroutine_open(void) {
	schedule *S = malloc(sizeof(schedule) + btree_size(4096));
    S->nco_id = 0LU;
    S->nco_alive = 0LU;
    S->blocked = 0LU;
    deque_init(&S->lock);
    deque_init(&S->cond);
    queue_init(&S->ready);
    S->running = NULL;
    skynet_timer_init(&S->TI);
    S->epfd = epoll_create(1);
    btree *p = S->tree;
    btree_create(4096,&p);
	return S;
}

uint64_t coroutine_new(schedule *S, coroutine_func func, void *arg){
    coroutine *co = malloc(sizeof(coroutine));
    co->id = S->nco_id++;
    co->func = func;
    co->arg = arg;
    co->fd = -1;
    co->cap = co->len = 0;
    co->stack = NULL;
    co->rc = 0;
    co->co_errno = 0;
    deque_init(&co->lock_node);
    deque_init(&co->timer_node);
    
    // 一开始需要加入就绪队列进行初始化
    co->status = COROUTINE_INIT;
    queue_push(&S->ready,&co->ready_node);

    S->nco_alive++;
    btree_insert(S->tree,co->id,co,1);
    return co->id;
}

uint64_t coroutine_id(schedule * S){
    return S->running->id;
}

int coroutine_yield(schedule * S){
    coroutine *co = S->running;
    // 当前没有运行
    S->running = NULL;
    // 重新加回ready队列
    co->status = COROUTINE_READY;
    queue_push(&S->ready,&co->ready_node);
    // 保存栈
    save_stack();
    // 切换
    swapcontext(&co->ctx,&S->main);
    return 0;
}

int coroutine_wait(schedule * S){
    coroutine *co = S->running;
    // 当前没有运行
    S->running = NULL;
    // 阻塞状态
    co->status = COROUTINE_BLOCKED;
    co->rc = 0;
    co->co_errno = 0;
    S->blocked++;
    // 保存栈
    save_stack();
    // 切换
    swapcontext(&co->ctx,&S->main);
    errno = co->co_errno;
    return co->rc;
}

int coroutine_io_ctl(schedule *S, int fd, int opt, unsigned int events){
    coroutine *co = S->running;
    struct epoll_event ev = {.data.ptr = co,.events = events};
    return epoll_ctl(S->epfd,opt,fd,&ev);
}

int coroutine_alarm(schedule *S, unsigned int seconds){
    coroutine *co = S->running;

    if((seconds/10) == 0){
        deque_remove(&co->timer_node);
        return 0;
    }

    timer_add(&S->TI,&co->timer_node,seconds);
    return 0;
}

int coroutine_wake(schedule *S,uint64_t id){
    coroutine *co = NULL;
    if(btree_search(S->tree,id,&co) && co->status == COROUTINE_BLOCKED){
        queue_push(&S->ready,&co->ready_node); // 加入就绪队列
        co->status = COROUTINE_READY; // 更新状态
        S->blocked--;
        return 0;
    }
    return -1;
}

void resume_handle(schedule *S,coroutine *co){
    co->func(S,co->arg);
    co->status = COROUTINE_DEAD;
    queue_push(&S->ready,&co->ready_node);
    S->running = NULL;
}

static void _ioloop(schedule *S,void *arg){
    struct epoll_event events[MAXEVENT];
    int i,ev_nready,timeout = -1;
    coroutine *co;
    struct timer_node head,*node;

    while(!queue_empty(&S->ready) || S->blocked){
        if(S->blocked){
            // 获取超时协程
            deque_init(&head);
            skynet_updatetime(&S->TI,&head);
            while(!deque_empty(&head)){
                node = deque_pop(&head);
                co = deque_data(node,coroutine,timer_node);

                if(co->status == COROUTINE_BLOCKED){
                    queue_push(&S->ready,&co->ready_node); // 加入就绪队列
                    co->status = COROUTINE_READY; // 更新状态
                    S->blocked--;
                    co->rc = -1;
                    co->co_errno = ETIMEDOUT;
                }
            }

            if(!queue_empty(&S->ready)){
                timeout = 0;
            }else{
                timeout = timer_near(&S->TI);
            }

            ev_nready = epoll_wait(S->epfd,events,MAXEVENT,timeout);
            for(i=0;i<ev_nready;i++){
                co = (coroutine *)events[i].data.ptr;
                
                if(co->status == COROUTINE_BLOCKED){
                    if((events[i].events & EPOLLHUP) || (events[i].events & EPOLLRDHUP)){
                        co->rc = -1;
                        co->co_errno = ECONNRESET;
                    }
                    queue_push(&S->ready,&co->ready_node); // 加入就绪队列
                    co->status = COROUTINE_READY; // 更新状态
                    S->blocked--;
                }
            }
        }
        coroutine_yield(S);
    }
}

int coroutine_loop(schedule *S){
    coroutine_new(S,_ioloop,NULL);
    queue_node_t *ready_node;
    coroutine *co;

    while(S->nco_alive){
        ready_node = queue_pop(&S->ready);// epoll一直存在
        co = queue_data(ready_node,coroutine,ready_node);
        S->running = co;

        if(co->status == COROUTINE_INIT){
            co->status = COROUTINE_RUNNING;
            memset(S->stack,0,STACK_SIZE);

            getcontext(&co->ctx);
            co->ctx.uc_stack.ss_sp = S->stack;
            co->ctx.uc_stack.ss_size = STACK_SIZE;
            co->ctx.uc_link = &S->main;
            
            makecontext(&co->ctx,(void (*)(void))resume_handle,2,S,co);
            swapcontext(&S->main,&co->ctx);
        }else if(co->status == COROUTINE_READY){
            co->status = COROUTINE_RUNNING;
            memcpy(S->stack + STACK_SIZE - co->len,co->stack,co->len);
            swapcontext(&S->main,&co->ctx);
        }else{
            deque_remove(&co->lock_node);
            deque_remove(&co->timer_node);
            btree_delete(S->tree,co->id,NULL);
            free(co->stack);
            free(co);
            S->nco_alive--;
        }
    }
    return 0;
}

void coroutine_close(schedule *S) {
    btree_free(S->tree,1);
    close(S->epfd);
	free(S);
}

// 小伙伴们发问了：我们写c语言程序的malloc()是不是会调用到内核里的kmalloc()?

// 虽然名字很像，但是这是不一样的东西，大家可千万不要混淆啊。

// kmalloc()是提供给linux内核的各组件申请连续的物理内存用的；
// 而malloc()是提供给用户层的应用态的应用程序申请内存用的。
// 可不是一个东西，实现机制也大大不同。

// kmalloc()是基于伙伴算法和slab实现内存的申请，
// 而malloc() 是 C 标准库提供的内存分配函数，
// 对应到系统调用上，有两种实现方式，即 brk() 和 mmap()。
// 对小块内存（小于 128K），C 标准库使用 brk() 来分配，
// 也就是通过移动堆顶的位置来分配内存。
// 这些内存释放后并不会立刻归还系统，而是被缓存起来，这样就可以重复使用。
// 而大块内存（大于 128K），则直接使用内存映射 mmap() 来分配，
// 也就是在文件映射段找一块空闲内存分配出去。