#include <stdio.h>          
#include <stdlib.h>         
#include <string.h>         
#include <unistd.h>         
#include <sys/types.h>     
#include <sys/socket.h>     
#include <netinet/in.h>    
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h> 
#include <sys/stat.h>

#define PORT 8080
#define MAXEVENTS 64
#define DOCUMENT_ROOT "/var/www/data"

typedef struct{
    int server_sock;
    int epfd;
    
    struct sockaddr_in addr;
    struct epoll_event event;
    struct epoll_event* events;
} ServerConfig;


void cleanup(ServerConfig *config){
    if (config->server_sock > 0){close(config->server_sock);}
    if (config->epfd > 0){close(config->epfd);}
    if (config->events){free(config->events);}
}
int non_block(int sock){
    int flags=fcntl(sock,F_GETFL,0);
    if (flags==-1) return 1;
    flags |= O_NONBLOCK;
    return fcntl(sock,F_SETFL,flags);
}
int init_server_config(ServerConfig* config){
    memset(&config->addr,0,sizeof(config->addr));
    memset(&config->event,0,sizeof(config->event));
    config->server_sock=socket(AF_INET,SOCK_STREAM,0);
    if (config->server_sock < 0){perror("main.c: init_server: Error Creating Server Sock"); return 1;}
    int one=1;
    if (setsockopt(config->server_sock,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one))< 0){perror("main.c init_server: Error Setting Sock Opt"); return 1;}
    if (non_block(config->server_sock) < 0){perror("main.c: init_server: Error Making Socket NonBlocking");return 1;} 
    config->addr.sin_family=AF_INET;
    config->addr.sin_port=htons(PORT);
    config->addr.sin_addr.s_addr=INADDR_ANY;
    if (bind(config->server_sock,(struct sockaddr*)&config->addr,sizeof(config->addr)) < 0){perror("main.c: Error Binding Server Socket");return 1;}
    if (listen(config->server_sock,10) < 0){perror("main.c: Error Listening On Socket");return 1;}
    config->epfd=epoll_create1(0);
    if (config->epfd < 0){perror("main.c: init_server: Error Create Socket for EPFD");return 1;}
    config->event.data.fd=config->server_sock;
    config->event.events=EPOLLIN;
    if (epoll_ctl(config->epfd,EPOLL_CTL_ADD,config->server_sock,&config->event) < 0){perror("main.c init_server: Error Adding Mask to EPFD");return 1;}
    config->events=malloc(MAXEVENTS*sizeof(struct epoll_event));
    if (!config->events){perror("main.c: init_server: Error Allocating Mem for Events");return 1;}
    memset(config->events,0,MAXEVENTS*sizeof(struct epoll_event));
    return 0;
}
int path_exists(char *full_path){
    struct stat st;
    if (stat(full_path,&st)==0){
        if (S_ISREG(st.st_mode)){
            return 0;
        } else{
            return 1;
        }
    } else{
        return 1;
    }
}
void send_all(int sock,const char* data,size_t len){
    size_t total=0;
    while(total<len){
        ssize_t n=write(sock,data+total,len-total);
        if(n<0){
            if(errno==EAGAIN||errno==EWOULDBLOCK) continue;
            perror("write"); break;
        }
        total+=n;
    }
}


void serve_file(int client_sock,char* full_path){
    int fd=open(full_path,O_RDONLY);
    if(fd<0){
        const char *msg="HTTP/1.1 404 Not Found\r\nContent-Length: 13\r\n\r\n404 Not Found";
        send_all(client_sock,msg,strlen(msg));
        return;
    }
    char header[256];
    snprintf(header,sizeof(header),"HTTP/1.1 200 OK\r\n\r\n");
    send_all(client_sock,header,strlen(header));
    char buf[8192];
    ssize_t n;
    while((n=read(fd,buf,sizeof(buf)))>0){
        send_all(client_sock,buf,n);
    }
    if(n<0) perror("read file");
    close(fd);
}

int handle_client(int client_sock,char* buff){
    char* line=strstr(buff,"\r\n");
    if (line){
        *line='\0';
    } else {return 1;}
    char method[8],path[256],version[16];
    if (!sscanf(buff,"%7s %255s %15s",method,path,version)){fprintf(stderr,"main.c: handle_client: Error Getting Path\n");return 1;}
    if (strstr(path,"..") != NULL){fprintf(stderr,"main.c: handle_client: Tried path escalation\n");return 1;}
    if (strcmp(path,"/")==0 || strcmp(path,"")==0){
        snprintf(path,sizeof(path),"/index.html");
    }
    char full_path[512];
    memset(full_path,0,sizeof(full_path));
    int n=snprintf(full_path,sizeof(full_path),"%s%s",DOCUMENT_ROOT, path);
    if (n<0||n>=(int)sizeof(full_path)) return 1;
    printf("Path: %s\n",full_path);
    if (path_exists(full_path)){
        const char *msg="HTTP/1.1 404 Not Found\r\nContent-Length: 13\r\n\r\n404 Not Found";
        send_all(client_sock, msg, strlen(msg));
        return 1;
    }
    serve_file(client_sock,full_path);
    return 0;

}
int main(){
    ServerConfig server_config={0};
    if (init_server_config(&server_config)){cleanup(&server_config);return 1;}

    while (1){
        int n=epoll_wait(server_config.epfd,server_config.events,MAXEVENTS,50);
        if (0 < n){
            for (int i=0;i < n;i++){
                if (server_config.events[i].data.fd==server_config.server_sock){
                    while(1){
                        int client=accept(server_config.server_sock,NULL,NULL);
                        if (client < 0){
                            if (errno == EAGAIN || errno==EWOULDBLOCK){
                                break;
                            } else{
                                perror("main.c: Error Accepting Client Sock");
                                break;
                            }
                        } 
                        if (non_block(client) < 0){perror("main.c: Error Making Client Sock Non Blockk");break;}
                        struct epoll_event ev={0};
                        ev.data.fd=client;
                        ev.events=EPOLLIN;
                        if (epoll_ctl(server_config.epfd,EPOLL_CTL_ADD,client,&ev) < 0){perror("error adding client socket to epoll");break;}
                    }
                } else{
                    char buff[4096];
                    int r=read(server_config.events[i].data.fd,buff,sizeof(buff));
                    if (r <= 0){
                        close(server_config.events[i].data.fd);
                        epoll_ctl(server_config.epfd,EPOLL_CTL_DEL,server_config.events[i].data.fd,NULL);
                    } else{
                        buff[r]='\0';
                        handle_client(server_config.events[i].data.fd,buff);
                        close(server_config.events[i].data.fd);
                        epoll_ctl(server_config.epfd,EPOLL_CTL_DEL,server_config.events[i].data.fd,NULL);
                    }
                }
            }
        } else if ( n < 0){perror("main.c: Error Calling Epoll Wait");cleanup(&server_config);return 1;}
    }
    return 0;
}


