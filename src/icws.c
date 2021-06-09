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

#define MAXBUF 4096

int port; 
char rootDir[MAXBUF];
typedef struct sockaddr SA;

void get_cli_argument(int argc, char **argv) {
    int ch;
    static struct option long_options[] = {
        {"port", required_argument, NULL, 'p'},
        {"root", required_argument, NULL, 'r'},
        {NULL, 0, NULL, 0}
    };

    // loop over all of the options
    while ((ch = getopt_long(argc, argv, "p:r:", long_options, NULL)) != -1) {
        // check to see if a single character or long option came through
        switch (ch) {
            // short option 't'
            case 'p':
                port = atoi(optarg);
                break;
            // short option 'a'
            case 'r':
                strcpy(rootDir, optarg);
                break;
        }
    }
}

const char* get_MIME(char * filename) {
    char* tmp = NULL;
    const char *file_ext = strrchr(filename, '.');
    if(!file_ext || file_ext == filename) return tmp;

    if(!strcmp(file_ext, ".html"))
        tmp = "text/html";
    else if (!strcmp(file_ext, ".css"))
        tmp = "text/css";
    else if (!strcmp(file_ext, ".js"))
        tmp = "text/javascript";
    else if (!strcmp(file_ext, ".png"))
        tmp = "image/png";
    else if (!strcmp(file_ext, ".jpg") || !strcmp(file_ext, ".jpeg"))
        tmp = "image/jpg";
    else if (!strcmp(file_ext, ".gif"))
        tmp = "image/gif";
    else if (!strcmp(file_ext, ".txt"))
        tmp = "text/plain";
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
            "Content-Type: %s\r\n"
            "Last-Modified: %s\r\n\r\n", strlen(msg));
    write_all(connFd, buf, strlen(buf));
    write_all(connFd, msg, strlen(msg));
    free(msg);
}

void get_file(char* filename, int connFd, int writeBody) {
    char *mime_type;
    char fullDir[MAXBUF];

    strcpy(fullDir, rootDir);
    strcat(fullDir, filename);
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
            "Content-length: %lu\r\n"
            "Content-type: %s\r\n\r\n", fileSize, mime_type);

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

void read_request(int connFd) {
    char buf[8192];
    int readRet = read(connFd, buf, 8192);
    // Parse the buffer to the parse function. 
    // You will need to pass the socket fd and the buffer would need to
    // be read from that fd
    Request *request = parse(buf, readRet, connFd);
    //Just printing everything
    if(request != NULL) { 
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
        response_400(connFd);
    }
}

int start_server() {
    int listenFd = open_listenfd(port);
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
    get_cli_argument(argc, argv);
    start_server();
}




