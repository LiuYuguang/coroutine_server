#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/epoll.h>
#include "coroutine.h"
#include "btree.h"

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
int localfd = -1;
btree *tree = NULL;

inline void setnonblock(int fd){
    int flags=0;
    flags = fcntl(fd,F_GETFL);
    assert(fcntl(fd,F_SETFL,flags|O_NONBLOCK) == 0);
}

inline void setreuse(int fd){
    int sockopt = 1;
    assert(setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&sockopt,sizeof(sockopt)) == 0);
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

ssize_t server_send(schedule* S,int sockfd, const void *buf, size_t len, int flags, int once){
    ssize_t rc = -1;
    
    again:

    rc = send(sockfd, buf, len, flags);
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

ssize_t server_recv(schedule* S, int sockfd, void *buf, size_t len, int flags, int once){
    ssize_t rc = -1;
    
    again:

    rc = recv(sockfd, buf, len, flags);
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
        rc = server_recv(S,srcfd,tmp,n,0,1);
        if(rc == -1){
            break;
        }

        n = rc;
        if(destfd >= 0){
            rc = server_send(S,destfd,tmp,n,0,1);
            if(rc == -1){
                break;
            }
        }
        
        len -= n;
        total += n;
    }

    return total;
}

void remote_client_handle(schedule* S,void* arg){
    uint64_t co_id = coroutine_id(S);
    int sockfd = *(int*)arg,rc;
    char tmp[1024];
    ssize_t len;
    Header header;

    // printf("%s start %lu\n",__FUNCTION__,co_id);

    // 接收数据
    coroutine_alarm(S,1000);// 读数据1秒
    len = server_recv(S, sockfd, tmp, sizeof(tmp), 0, 1);
    // printf("fd %d rc %ld\n", sockfd, len);
    coroutine_alarm(S,0);

    if(len <=0 ){
        // printf("recv fail %ld\n",len);
        goto finish;
    }

    // 发送给local
    coroutine_alarm(S,10*1000);// 发数据10秒
    if(coroutine_cond_acquire(S) == -1){
        // printf("lock fail\n");
        goto finish;
    }
    // printf("lock success\n");

    while(localfd == -1 && (rc=coroutine_cond_wait(S)) == 0);
    if(rc != 0){
        // printf("wait fail\n");
        goto finish;
    }

    // printf("wait success\n");

    // printf("send local\n");

    header.id = co_id;
    header.len = len;
    server_send(S,localfd,&header,sizeof(header),0,1);
    server_send(S,localfd,tmp,header.len,0,1);

    // printf("send local finish\n");

    coroutine_cond_notify(S);// 唤醒下一个
    coroutine_cond_release(S);
    coroutine_alarm(S,0);// 设置关闭触发

    // 等待sock关闭 
    coroutine_alarm(S,1000);// 设置5秒后触发
    coroutine_wait(S);
    coroutine_alarm(S,0);// 设置关闭触发
    shutdown(sockfd,SHUT_RDWR);

    finish:
    // printf("%s finish\n",__FUNCTION__);

    // 释放map
    btree_delete(tree,co_id,NULL);
    free(arg);
    // 关闭sock
    close(sockfd);
}

void remote_server(schedule* S,void* arg){
    int listenfd = -1;
    int sockfd;
    uint64_t co_id;
    char ipaddress[50];
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    listenfd = socket(AF_INET,SOCK_STREAM,0);
    
    // 复用
    setreuse(listenfd);
    // 非阻塞IO
    setnonblock(listenfd);

    addr.sin_family = AF_INET;
	addr.sin_port = htons((int)arg);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
    assert(bind(listenfd,(struct sockaddr*)&addr,sizeof(addr)) == 0);
    assert(listen(listenfd,REMOTE_BACKLOG) == 0);

    coroutine_io_ctl(S,listenfd,EPOLL_CTL_ADD,EPOLLIN);
    while((sockfd = server_accept(S,listenfd,(struct sockaddr*)&addr,&addr_len,0)) >= 0){
        // printf("accpet fd %d %s:%d\n",sockfd,inet_ntop(AF_INET,&addr.sin_addr,ipaddress,sizeof(ipaddress)),ntohs(addr.sin_port));

        // 非阻塞
        setnonblock(sockfd);
        int *arg = malloc(sizeof(int));
        *arg = sockfd;
        co_id = coroutine_new(S,remote_client_handle,arg);
        btree_insert(tree,co_id,(intptr_t)arg,1);
    };
    coroutine_io_ctl(S,listenfd,EPOLL_CTL_DEL,EPOLLIN);
    close(listenfd);
}

void local_server(schedule* S,void* arg){
    int listenfd = -1;
    int sockfd, remotefd, *pfd;
    struct sockaddr_un addr;
    socklen_t addr_len = sizeof(addr);

    ssize_t len;
    Header header;

    unlink(LOCAL_FILE);

    listenfd = socket(AF_UNIX, SOCK_STREAM, 0);
    // 复用
    setreuse(listenfd);
    // 非阻塞IO
    setnonblock(listenfd);

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path,LOCAL_FILE);
    assert(bind(listenfd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    assert(listen(listenfd, LOCAL_BACKLOG) == 0);

    while((sockfd = server_accept(S,listenfd,(struct sockaddr*)&addr,&addr_len,1)) >= 0){
        // 非阻塞
        setnonblock(sockfd);
        
        coroutine_cond_acquire(S);// 加锁
        localfd = dup(sockfd);
        coroutine_cond_notify(S);// 唤醒
        coroutine_cond_release(S);// 解锁

        for(;;){
            len = server_recv(S, sockfd, &header, sizeof(header), 0, 1);
            if(len <= 0){
                fprintf(stderr,"errno%d, strerror%s\n",errno,strerror(errno));
                break;
            }
            // printf("header id %lu\n",header.id);
            if(btree_search(tree,header.id,(intptr_t*)&pfd) == 1){
                remotefd = *pfd;
            }else{
                remotefd = -1;
            }
            // printf("local remotefd %d\n",remotefd);

            io_copy(S, remotefd, sockfd, header.len);
            coroutine_wake(S, header.id);// 唤醒协程
        }

        coroutine_cond_acquire(S);
        close(sockfd);
        close(localfd);
        localfd = -1;
        coroutine_cond_release(S);
    }

    close(listenfd);
}

int main(){
    dummyfd = open("/dev/null",O_RDONLY | O_CLOEXEC);
    assert(dummyfd >= 0);

    assert(btree_create(4096,&tree) == 0);

    schedule *S = coroutine_open();
    assert(S);

    short port;
    for(port=8890;port<8900;port++){
        coroutine_new(S,remote_server,port);    
    }
    coroutine_new(S,local_server,NULL);
    coroutine_loop(S);
    coroutine_close(S);
    return 0;
}