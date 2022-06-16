#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h> //for socket
#include <netinet/in.h> //for sockaddr_in htons htonl
#include <arpa/inet.h>  //for htons htonl
#include <sys/un.h>     //for struct sockaddr_un

#include <errno.h>
#include <string.h>

#include <assert.h>

#include "async_server.h"

typedef struct{
    uint64_t id;
    size_t len;
    unsigned char type;
    unsigned char data[0];
}Header;

#define RESPONSE "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n"

int main(){
    const char socket_file[] = "/tmp/socksdtcp.sock";
    int sockfd;
    struct sockaddr_un addr;
    Header header;
    ssize_t len;
    size_t need;

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path,socket_file);
    sockfd = socket(AF_UNIX,SOCK_STREAM,0);
    assert(sockfd >= 0);

    assert(connect(sockfd,(struct sockaddr*)&addr,sizeof(addr)) == 0);

    unsigned char tmp[1024];

    for(;;){
        len = recv(sockfd, &header, sizeof(header),0);
        if(len == 0){
            fprintf(stderr,"close by peer\n");
            break;
        }else if(len == -1){
            fprintf(stderr,"errno%d, strerror%s\n",errno,strerror(errno));
            break;
        }

        while(header.len > 0){
            need = header.len < sizeof(tmp) ? header.len : sizeof(tmp);
            len = recv(sockfd,tmp,need,0);
            if(len == 0){
                fprintf(stderr,"close by peer\n");
                break;
            }else if(len == -1){
                fprintf(stderr,"errno%d, strerror%s\n",errno,strerror(errno));
                break;
            }
            // printf("recv %.*s\n",(int)len,tmp);
            header.len -= len;
        }

        header.len = sizeof(RESPONSE)-1;
        len = send(sockfd,&header,sizeof(header),0);
        if(len == -1){
            fprintf(stderr,"errno%d, strerror%s\n",errno,strerror(errno));
            break;
        }
        len = send(sockfd,RESPONSE,sizeof(RESPONSE)-1,0);
        if(len == -1){
            fprintf(stderr,"errno%d, strerror%s\n",errno,strerror(errno));
            break;
        }
        // printf("send finish\n");
    }

    close(sockfd);
    return 0;
}
