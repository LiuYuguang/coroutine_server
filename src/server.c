#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/epoll.h>
#include "coroutine.h"
#include "btree.h"
#include "deque.h"

#define REMOTE_BACKLOG 128
#define REMOTE_PORT 8888
#define LOCAL_BACKLOG 1
#define LOCAL_FILE "/tmp/socksdtcp.sock"

typedef struct{
    uint64_t id;
    size_t len;
    unsigned char type;
    unsigned char data[0];
}Header;

int dummyfd = -1;
btree *tree = NULL;
deque_t task_queue, worker_queue;

typedef struct{
    uint64_t co_id;
    size_t len;
    char *data;
    deque_t task_node;
}Task;

typedef struct{
    uint64_t co_id;
    deque_t worker_node;
}Worker;

inline static int nonblock_socket(const int sock){
    int flags = fcntl(sock, F_GETFL);

    if (flags != -1){
        flags = fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }
    return flags;
}

inline static int reuse_socket(const int sock){
    int sockopt = 1;
    return setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,&sockopt,sizeof(sockopt));
}

int server_accept(schedule* S, int sockfd, struct sockaddr *addr, socklen_t *addrlen, int once){
    int rc = -1;

    again:

    rc = accept(sockfd, addr, addrlen);
    if(rc == -1){
        if(errno == EINTR){
            //被信号中断, 下次继续读
            goto again;
        }else if(errno == ENFILE || errno == EMFILE){
            //fd不够分配，应该close, run out of file descriptors
            close(dummyfd);
            rc = accept(sockfd, addr, addrlen);
            if(rc >= 0){
                close(rc);
            }
            dummyfd = open("/dev/null",O_RDONLY | O_CLOEXEC);
            goto again;
        }else if(errno == EAGAIN || errno == EWOULDBLOCK){
            //数据未准备好, 下次继续读
            if(once)    coroutine_io_ctl(S, sockfd, EPOLL_CTL_ADD, EPOLLIN);
            rc = coroutine_wait(S);
            if(once)    coroutine_io_ctl(S, sockfd, EPOLL_CTL_DEL, EPOLLIN);
            if(rc == 0){
                // printf("accept continue\n");
                goto again;
            }else{
                fprintf(stderr,"accept fail errno[%d]%s\n",errno,strerror(errno));
            }
            goto again;
        }else{
            fprintf(stderr,"accept fail errno[%d]%s\n",errno,strerror(errno));
        }
    }
    return rc;
}

ssize_t server_send(schedule* S,int sockfd, const void *buf, size_t len, int once){
    ssize_t rc = -1;
    
    again:

    rc = send(sockfd, buf, len, 0);
    if(rc == -1){
        if(errno == EINTR){
            // 被信号中断, 下次继续读
            goto again;
        }else if(errno == EAGAIN || errno == EWOULDBLOCK){
            // 写缓存满
            if(once)    coroutine_io_ctl(S,sockfd,EPOLL_CTL_ADD,EPOLLOUT);
            rc = coroutine_wait(S);
            if(once)    coroutine_io_ctl(S,sockfd,EPOLL_CTL_DEL,EPOLLOUT);
            if(rc == 0){
                // printf("send continue\n");
                goto again;
            }else{
                fprintf(stderr,"send fail errno[%d]%s\n",errno,strerror(errno));
            }
        }else{
            fprintf(stderr,"send fail errno[%d]%s\n",errno,strerror(errno));
        }
    }
    return rc;
}

ssize_t server_recv(schedule* S, int sockfd, void *buf, size_t len, int once){
    ssize_t rc = -1;
    
    again:

    rc = recv(sockfd, buf, len, 0);
    if(rc == 0){
        fprintf(stderr,"close by peer errno[%d]%s\n",errno,strerror(errno));
    }else if(rc == -1){
        if(errno == EINTR){
            // 被信号中断, 下次继续读
            goto again;
        }else if(errno == EAGAIN || errno == EWOULDBLOCK){
            // 未有数据
            if(once)    coroutine_io_ctl(S,sockfd,EPOLL_CTL_ADD,EPOLLIN);
            rc = coroutine_wait(S);
            if(once)    coroutine_io_ctl(S,sockfd,EPOLL_CTL_DEL,EPOLLIN);
            if(rc == 0){
                // printf("recv continue\n");
                goto again;
            }else{
                fprintf(stderr,"recv fail errno[%d]%s\n",errno,strerror(errno));
            }
        }else{
            fprintf(stderr,"recv fail errno[%d]%s\n",errno,strerror(errno));
        }
    }
    
    return rc;
}

ssize_t io_copy(schedule* S, int destfd, int srcfd, size_t len){
    unsigned char tmp[1024];
    size_t n,total=0;
    ssize_t rc;

    while(len){
        n = len < sizeof(tmp) ? len : sizeof(tmp);
        rc = server_recv(S,srcfd,tmp,n,1);
        if(rc == -1){
            break;
        }

        n = rc;
        if(destfd >= 0){
            rc = server_send(S,destfd,tmp,n,1);
            if(rc == -1){
                destfd = -1;
            }
        }
        
        len -= n;
        total += n;
    }

    return total;
}

void remote_client_handle(schedule* S,void* arg){
    uint64_t co_id = coroutine_id(S);
    int sockfd = *(int*)arg;
    char *buf = malloc(1024);
    ssize_t len;
    Task *task = NULL;

    // 接收数据
    coroutine_alarm(S,1000);// 读数据1秒
    len = server_recv(S, sockfd, buf, 1024, 1);
    coroutine_alarm(S,0);
    
    if(len <= 0){
        goto finish;
    }

    btree_insert(tree,co_id,(intptr_t)arg,1);

    task = malloc(sizeof(Task));
    task->co_id = co_id;
    task->data = buf;
    task->len = len;
    deque_push(&task_queue,&task->task_node);

    if(!deque_empty(&worker_queue)){
        deque_t *node = deque_pop(&worker_queue);
        Worker *woker = deque_data(node,Worker,worker_node);
        coroutine_wake(S, woker->co_id);
    }

    coroutine_alarm(S,10*1000);
    coroutine_io_ctl(S,sockfd,EPOLL_CTL_ADD,EPOLLRDHUP);
    coroutine_wait(S);
    coroutine_alarm(S,0);
    
    btree_delete(tree,co_id,NULL);
    deque_remove(&task->task_node);

    finish:
    free(buf);
    free(task);
    free(arg);
    close(sockfd);
}

void remote_server(schedule* S,void* arg){
    int listenfd = -1;
    int sockfd;
    char ipaddress[50];
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(*(short*)arg), .sin_addr.s_addr = htonl(INADDR_ANY)};
    socklen_t addr_len = sizeof(addr);
    free(arg);

    listenfd = socket(AF_INET,SOCK_STREAM,0);
    reuse_socket(listenfd);
    nonblock_socket(listenfd);
    bind(listenfd,(struct sockaddr*)&addr,sizeof(addr));
    listen(listenfd,REMOTE_BACKLOG);

    coroutine_io_ctl(S,listenfd,EPOLL_CTL_ADD,EPOLLIN);
    while((sockfd = server_accept(S,listenfd,(struct sockaddr*)&addr,&addr_len,0)) >= 0){
        nonblock_socket(sockfd);

        int *pfd = malloc(sizeof(int));
        *pfd = sockfd;
        coroutine_new(S,remote_client_handle,pfd);
    };
    coroutine_io_ctl(S,listenfd,EPOLL_CTL_DEL,EPOLLIN);
    close(listenfd);
}

void local_send(schedule* S,void* arg){
    int sockfd = *(int*)arg, rc;
    free(arg);
    uint64_t co_id = coroutine_id(S);
    char *buf=NULL;
    size_t cap=0;

    Worker *woker = malloc(sizeof(Worker));
    woker->co_id = co_id;
    deque_init(&woker->worker_node);

    deque_t *node;
    for(;;){
        while(deque_empty(&task_queue)){
            coroutine_io_ctl(S,sockfd,EPOLL_CTL_ADD,EPOLLRDHUP);
            deque_push(&worker_queue,&woker->worker_node);
            rc = coroutine_wait(S);
            coroutine_io_ctl(S,sockfd,EPOLL_CTL_DEL,EPOLLRDHUP);
            deque_remove(&woker->worker_node);
            if(rc == -1){
                goto finish;
            }
        }

        node = deque_pop(&task_queue);
        Task *task = deque_data(node, Task, task_node);

        if(cap < sizeof(Header) + task->len){
            char *p = buf;
            buf = realloc(buf,sizeof(Header) + task->len);
            if(buf == NULL){
                buf = p;
                continue;
            }
            cap = sizeof(Header) + task->len;
        }
        Header *header = (Header *)buf;
        header->id = task->co_id;
        header->len = task->len;
        memcpy(header->data,task->data,task->len);
        server_send(S, sockfd,buf,sizeof(Header) + task->len,1);
    }
    finish:
    free(buf);
    free(woker);
    close(sockfd);
}

void local_recv(schedule* S,void* arg){
    int sockfd = *(int*)arg, rc;
    free(arg);
    ssize_t len;
    Header header;

    for(;;){
        len = server_recv(S, sockfd, &header, sizeof(header), 1);
        if(len <= 0){
            break;
        }
        int *p = NULL;
        int remote_fd = -1;
        if(btree_search(tree,header.id,(intptr_t*)&p) == 1){
            remote_fd = *p;
        }else{
            remote_fd = -1;
        }
        io_copy(S,remote_fd,sockfd,header.len);
        coroutine_wake(S,header.id);
    }
    close(sockfd);
}

void local_server(schedule* S,void* arg){
    int listenfd = -1;
    int sockfd, *pfd;
    struct sockaddr_un addr = {.sun_family=AF_UNIX, .sun_path = LOCAL_FILE};
    socklen_t addr_len = sizeof(addr);
    
    unlink(LOCAL_FILE);

    listenfd = socket(AF_UNIX, SOCK_STREAM, 0);
    reuse_socket(listenfd);
    nonblock_socket(listenfd);
    bind(listenfd, (struct sockaddr*)&addr, sizeof(addr));
    listen(listenfd, LOCAL_BACKLOG);

    coroutine_io_ctl(S,listenfd,EPOLL_CTL_ADD,EPOLLIN);
    while((sockfd = server_accept(S,listenfd,(struct sockaddr*)&addr,&addr_len,0)) >= 0){
        nonblock_socket(sockfd);

        pfd = malloc(sizeof(int));
        *pfd = sockfd;
        coroutine_new(S,local_send,pfd);

        sockfd = dup(sockfd);
        pfd = malloc(sizeof(int));
        *pfd = sockfd;
        coroutine_new(S,local_recv,pfd);
    }
    coroutine_io_ctl(S,listenfd,EPOLL_CTL_DEL,EPOLLIN);
    close(listenfd);
}

int main(){
    setlinebuf(stdout);
    setlinebuf(stderr);

    dummyfd = open("/dev/null",O_RDONLY | O_CLOEXEC);

    btree_create(4096,&tree);

    deque_init(&task_queue);
    deque_init(&worker_queue);

    schedule *S = coroutine_open();

    short port;
    for(port=8890;port<8900;port++){
        short *p = malloc(sizeof(short));
        *p = port;
        coroutine_new(S,remote_server,p);
    }
    coroutine_new(S,local_server,NULL);
    coroutine_loop(S);
    coroutine_close(S);
    return 0;
}