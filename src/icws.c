#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include<sys/socket.h>
#include<netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include "parse.h"
#include "pcsa_net.h"

#define MAX_HEADER_BUF 8192
#define MAXBUF 4096

char port[MAXBUF]; 
char rootDir[MAXBUF];
typedef struct sockaddr SA;

void get_cli_argument(int argc, char **argv) {
    printf("PARSING!\n");
    int ch;
    static struct option long_options[] = {
        {"port", required_argument, NULL, 'p'},
        {"root", required_argument, NULL, 'r'},
        {NULL, 0, NULL, 0}
    };
    printf("running loop\n");
    // loop over all of the options
    while ((ch = getopt_long(argc, argv, "p:r:", long_options, NULL)) != -1) {
        // check to see if a single character or long option came through
        switch (ch) {
            // short option 't'
            case 'p':
                printf("port: %s\n", optarg);
                strcpy(port, optarg);
                break;
            // short option 'a'
            case 'r':
                printf("rootdir: %s\n", optarg);
                strcpy(rootDir, optarg);
                break;
        }
    }
}

char* get_MIME(char * filename) {
    const char *file_ext = strrchr(filename, '.');
    if(!file_ext || file_ext == filename) return NULL;
    char* tmp = malloc(sizeof(char) * MAXBUF);
    if(!strcmp(file_ext, ".html"))
        strcpy(tmp, "text/html");
    else if (!strcmp(file_ext, ".css"))
        strcpy(tmp, "text/css");
    else if (!strcmp(file_ext, ".js"))
        strcpy(tmp, "text/javascript");
    else if (!strcmp(file_ext, ".png"))
        strcpy(tmp,"image/png");
    else if (!strcmp(file_ext, ".jpg") || !strcmp(file_ext, ".jpeg"))
        strcpy(tmp,"image/jpg");
    else if (!strcmp(file_ext, ".gif"))
        strcpy(tmp,"image/gif");
    else if (!strcmp(file_ext, ".txt"))
        strcpy(tmp,"text/plain");
    else
        return NULL;
    return tmp;
}

void response_template(char* buf, int status) {
    char status_message[MAXBUF];
    switch (status) {
        case 200:
            strcpy(status_message, "OK");
            break;
        case 404:
            strcpy(status_message, "Not Found");
            break;
        case 501:
            strcpy(status_message, "Not Implemented");
            break;
    }
    sprintf(buf,
            "HTTP/1.1 %d %s\r\n"
            "Server: ICWS\r\n"
            "Connection: close\r\n",
            status,
            status_message);
}

void response_404(int connFd) {
    char buf[MAXBUF];
    char *msg = "<h1>404 Content not found</h1> ";
    response_template(buf, 404);
    sprintf(buf + strlen(buf),   
            "Content-Length: %lu\r\n"
            "Content-Type: text/html\r\n", strlen(msg));
    write_all(connFd, buf, strlen(buf));
    write_all(connFd, msg, strlen(msg));
    free(msg);
}

void response_file(int inputFd, int connFd, int writeBody, char* mime_type) {
    char buf[MAXBUF];
    struct stat statBuffer;
    struct timespec last_modified;
    fstat(inputFd, &statBuffer);
    int fileSize = statBuffer.st_size;
    last_modified = statBuffer.st_ctim;
    printf("%s", ctime(&statBuffer.st_mtime));
    response_template(buf, 200);
    sprintf(buf + strlen(buf), 
            "Content-Length: %d\r\n"
            "Content-Type: %s\r\n\r\n", fileSize, mime_type);

    write_all(connFd, buf, strlen(buf));
    if(writeBody) {
        ssize_t numRead;
        char fileBuf[MAXBUF];
        while ((numRead = read(inputFd, fileBuf, MAXBUF)) > 0) {
            char *tmpBuf = fileBuf;
            while(numRead > 0) {
                ssize_t numWritten = write(connFd, tmpBuf, numRead);
                numRead -= numWritten;
                tmpBuf += numWritten;
            }
        }
    }
}

void get_file(char* filename, int connFd, int writeBody) {
    char *mime_type;
    char fullDir[MAXBUF];

    strcpy(fullDir, rootDir);
    strcat(fullDir, filename);
    printf("Full dir path: %s\n", fullDir);
    int inputFd = open(fullDir, O_RDONLY);
    if(inputFd < 0 || (mime_type=get_MIME(filename)) != NULL) {
        printf("File not found!\n");
        response_404(connFd);
    } else {
        response_file(inputFd, connFd, writeBody, mime_type);
    }
}

void response_not_implemented(int connFd) {
    char responseBuf[MAXBUF];
    response_template(responseBuf, 501);
    write_all(connFd, responseBuf, strlen(responseBuf));
}

void response_400(int connFd) {
    char responseBuf[MAXBUF];
    response_template(responseBuf, 400);
    write_all(connFd, responseBuf, strlen(responseBuf));
}

ssize_t get_request_buffer(int connFd, char* buf) {
    int totalRead = 0;
    int numRead;
    char *bufp = buf;
    printf("BUFFERING REQUEST....\n");
    while(totalRead <= MAX_HEADER_BUF) {
        printf("TOTAL READ.....%d\n", totalRead);
        if((numRead = read(connFd, bufp, 256)) > 0) {
            totalRead += numRead;
            if(totalRead > MAX_HEADER_BUF) return -1;
            bufp += numRead;
        }
        else if(numRead == 0) 
            break;
        else 
            return -1;
    }
    return totalRead;
}


void read_request(int connFd) {
    printf("Reading request\n");
    char buf[MAX_HEADER_BUF+555];
    ssize_t request_buf_len = get_request_buffer(connFd, buf);
    printf("Get buffer complete..\n");
    printf("buffer read is %ld\n", request_buf_len);
    Request *request;
    if(request_buf_len > 0)
        request = parse(buf, request_buf_len, connFd);
    //Just printing everything
    if(request != NULL || request_buf_len > 0) { 
        char* http_method = request->http_method;
        char* http_uri = request->http_uri;
 
        if(!strcmp(http_method, "GET")) {
            get_file(http_uri, connFd, 1);
        }
        else if(!strcmp(http_method, "HEAD")) {
            get_file(http_uri, connFd, 0);
        }
        else {
            response_not_implemented(connFd);
        }
        for(int index = 0;index < request->header_count; index++){
            printf("Request Header\n");
            printf("Header name %s Header Value %s\n",request->headers[index].header_name,request->headers[index].header_value);
        }
        free(request->headers);
        free(request);
    } else {
        printf("Bad request...\n");
        response_400(connFd);
    }
}

int start_server() {
    int listenFd = open_listenfd(port);
    printf("STARTING SERVER!!!\n");
    for (;;) {
        struct sockaddr_storage clientAddr;
        socklen_t clientLen = sizeof(struct sockaddr_storage);

        int connFd = accept(listenFd, (SA *) &clientAddr, &clientLen);
        if (connFd < 0) { fprintf(stderr, "Failed to accept\n"); continue; }

        char hostBuf[MAXBUF], svcBuf[MAXBUF];
        if (getnameinfo((SA *) &clientAddr, clientLen, 
                        hostBuf, MAXBUF, svcBuf, MAXBUF, 0)==0) 
            printf("Connection from %s:%s\n", hostBuf, svcBuf);
        else
            printf("Connection from ?UNKNOWN?\n");
                
        read_request(connFd);
        close(connFd);
    }
}

int main(int argc, char **argv){
    //Read from the file the sample
    // https://stackoverflow.com/questions/7489093/getopt-long-proper-way-to-use-it
    printf("YEP THIS THING RUN IN MAIN!\n");
    get_cli_argument(argc, argv);
    start_server();
}




