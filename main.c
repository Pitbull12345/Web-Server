#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <limits.h>



#define PORT 8080
#define DOCUMENT_ROOT "/var/www/data"

typedef struct{
    int client_sock;
    struct sockaddr_in addr;
    socklen_t addrlen;
} ClientConfig;

typedef struct{
    int server_sock;
    struct sockaddr_in addr;
    socklen_t addrlen;
} ServerConfig;


int init_server(ServerConfig* config){
    memset(config,0,sizeof(*config));
    config->addrlen=sizeof(config->addr);
    config->server_sock=socket(AF_INET,SOCK_STREAM,0);
    if (config->server_sock< 0){
        perror("main.c: init_server: Error Creating Server Socket");
        return 1;
    }
    int opt=1;
    if (setsockopt(config->server_sock,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt)) < 0){
        perror("main.c: init_server: Error Setting Sock Opt on Server Sock");
        return 1;
    }
    config->addr.sin_family=AF_INET;
    config->addr.sin_port=htons(PORT);
    config->addr.sin_addr.s_addr=INADDR_ANY;
    if (bind(config->server_sock,(struct sockaddr*)&config->addr,config->addrlen)< 0){
        perror("main.c: init_server: Error Binding Server Socket Config");
        return 1;
    }
    if (listen(config->server_sock,10) < 0){
        perror("main.c: init_server: Error Listening on Server Socket");
        return 1;
    }
    return 0;
}
int get_path(char* full_path, size_t size, char *recv_ptr){
    char* line=strstr(recv_ptr,"\r\n");
    if (line){
        *line='\0';
    } else{
        return 1;
    }
    char method[8],path[256],version[16];
    sscanf(recv_ptr,"%7s %255s %15s",method,path,version);
    printf("Path: %s\n",path);
    if (strstr(path,"..")!=NULL){
        return 1;
    }

    int n = snprintf(full_path, size, "%s%s", DOCUMENT_ROOT, path);
    if (n < 0 || n >= (int)size) return 1;

    char resolved[PATH_MAX];
    if (!realpath(full_path, resolved)) return 1;

    if (strncmp(resolved, DOCUMENT_ROOT, strlen(DOCUMENT_ROOT)) != 0) return 1;

    struct stat st;
    if (stat(full_path, &st) != 0){
        perror("main.c: get_path: Path Not Found");
        return 1;
    }
    if (!S_ISREG(st.st_mode)){
        perror("main.c: get_path: Is Not Regular File");
        return 1;
    }


    return 0;
}

int handle_client(ServerConfig* server_config){
    //get path/verify then read path into buffer, contiously server to one client
    ssize_t bytes;
    char recv_buff[4096];
    ClientConfig client_config;
    memset(&client_config,0,sizeof(client_config));
    client_config.addrlen=sizeof(client_config.addr);
    client_config.client_sock=accept(server_config->server_sock,(struct sockaddr*)&client_config.addr,&client_config.addrlen);
    while (1){
        bytes=read(client_config.client_sock,recv_buff,sizeof(recv_buff)-1);
        if (bytes > 0){
            recv_buff[bytes]='\0';
            /////////
            char full_path[PATH_MAX];
            if(!get_path(full_path,sizeof(full_path),recv_buff)){
                printf("yes");
            } else{
                fprintf(stderr,"main.c: handle_client: Error With Path\n");
                close(client_config.client_sock);
                return 1;
            }
            printf("%s",recv_buff);
        } else if (bytes==0){
            printf("main.c: handle_client: Client Connection Closed\n");
            break;
        }
         else{
            perror("main.c: handle_client: Error Accepting Connection from Client");
            close(client_config.client_sock);
            return 1;
        }
    }
    close(client_config.client_sock);
    return 0;    
}



int main(){
    //init then do main loop then handle a client connection and serve then end
    ServerConfig server_config;
    init_server(&server_config);
    while (1){
        if(handle_client(&server_config)){break;}
    }
}