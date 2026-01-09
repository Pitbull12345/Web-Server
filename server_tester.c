#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define MAX_EVENTS 15
#define REQUEST "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n"
typedef enum {STATE_UNSENT,STATE_SENT,STATE_OPEN,STATE_CLOSED} ConnectionState;

typedef struct{
    unsigned short port;
    unsigned short total_request;
    unsigned short concurrent_requests;
    char* target_ip;
    
    int epfd;
    struct epoll_event* events;
} TesterConfig;

typedef struct{
    ConnectionState state;
    double clock_start;
    double clock_end;
    int socket_fd;
    struct sockaddr_in addr;
    struct epoll_event event;
} Connection;
void cleanup(TesterConfig* config,Connection* connections){
    if(connections){
        for(int i=0;i<config->concurrent_requests;i++){
            if(connections[i].socket_fd > 0){
                close(connections[i].socket_fd);
            }
        }
    }
    if(config->events){free(config->events);}
    if(config->epfd > 0){close(config->epfd);}
}
double current_time_ms(){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
int non_block(int sock){
    int flags=fcntl(sock,F_GETFL,0);
    if (flags==-1) return 1;
    flags |= O_NONBLOCK;
    return fcntl(sock,F_SETFL,flags);
}

int init_connection(TesterConfig* config,int i,Connection* connections){
    connections[i].socket_fd=socket(AF_INET,SOCK_STREAM,0);
    if(connections[i].socket_fd<0){perror("Error Creating Socket"); return 1;}
    if(non_block(connections[i].socket_fd)<0){perror("Error Making Socket NonBlock"); return 1;}
    connections[i].addr.sin_family=AF_INET;
    connections[i].addr.sin_port=htons(config->port);
    inet_pton(AF_INET,config->target_ip,&connections[i].addr.sin_addr);
    connections[i].event.data.fd=connections[i].socket_fd;
    connections[i].event.events=EPOLLIN|EPOLLOUT;
    if(epoll_ctl(config->epfd,EPOLL_CTL_ADD,connections[i].socket_fd,&connections[i].event)){
        perror("Error Adding Socket to Epoll"); return 1;
    }
    connections[i].state=STATE_UNSENT;
    connections[i].clock_start=0;
    connections[i].clock_end=0;
    return 0;
}


int create_epoll(TesterConfig* config){
    config->epfd=epoll_create1(0);
    if (config->epfd < 0){perror("error Creating epfd");return 1;}
    config->events = malloc(config->concurrent_requests * sizeof(struct epoll_event));
    if (!config->events){perror("Error Mallocing Events for Epoll");return 1;}
    return 0;
}
int start_connections(TesterConfig* config, Connection* connections){
    for(int i=0;i<config->concurrent_requests;i++){
        int r=connect(connections[i].socket_fd,(struct sockaddr*)&connections[i].addr,sizeof(connections[i].addr));
        if(r<0){
            if(errno!=EINPROGRESS){perror("connect"); return 1;}
        }
        connections[i].clock_start=current_time_ms();
    }
    return 0;
}

int run_test(TesterConfig* config, Connection* connections){
    int completed=0;
    while(completed<config->concurrent_requests){
        int n=epoll_wait(config->epfd,config->events,MAX_EVENTS,1000);
        if(n<0){perror("epoll_wait"); return 1;}

        for(int i=0;i<n;i++){
            int fd=config->events[i].data.fd;
            for(int j=0;j<config->concurrent_requests;j++){
                if(connections[j].socket_fd!=fd) continue;
                uint32_t ev=config->events[i].events;

                if((ev & EPOLLOUT) && connections[j].state==STATE_UNSENT){
                    size_t total=0,len=strlen(REQUEST);
                    while(total<len){
                        ssize_t w=write(fd,REQUEST+total,len-total);
                        if(w<0){
                            if(errno==EAGAIN||errno==EWOULDBLOCK) continue;
                            perror("write"); break;
                        }
                        total+=w;
                    }
                    connections[j].state=STATE_SENT;
                } else if((ev & EPOLLIN) && connections[j].state==STATE_SENT){
                    char buf[4096]; int r;
                    while((r=read(fd,buf,sizeof(buf)-1))>0){
                        buf[r]='\0';
                        connections[j].state=STATE_OPEN;
                    }
                    if(r==0){
                        connections[j].clock_end=current_time_ms();
                        connections[j].state=STATE_CLOSED;
                        close(fd);
                        epoll_ctl(config->epfd,EPOLL_CTL_DEL,fd,NULL);
                        completed++;
                    }
                }
            }
        }
    }
    return 0;
}
int main(){
    TesterConfig config;
    config.port=8080;
    config.total_request=100;
    config.concurrent_requests=10;
    config.target_ip="127.0.0.1";

    Connection connections[config.concurrent_requests];

    if(create_epoll(&config)){cleanup(&config,connections); return 1;}

    for(int i=0;i<config.concurrent_requests;i++){
        if(init_connection(&config,i,connections)){cleanup(&config,connections); return 1;}
    }

    if(start_connections(&config,connections)){cleanup(&config,connections); return 1;}
    if(run_test(&config,connections)){cleanup(&config,connections); return 1;}

    for(int i=0;i<config.concurrent_requests;i++){
        printf("Connection %d: %.2f ms\n",i,connections[i].clock_end-connections[i].clock_start);
    }

    cleanup(&config,connections);
    return 0;
}