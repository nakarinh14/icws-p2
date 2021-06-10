#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
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


static const char *DAY_NAMES[] =
  { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *MONTH_NAMES[] =
  { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

// https://stackoverflow.com/questions/2726975/how-can-i-generate-an-rfc1123-date-string-from-c-code-win32
char * parse_rfc_datetime(time_t * t) {
    const int RFC1123_TIME_LEN = 29;
    char * buf = malloc(RFC1123_TIME_LEN+1);
    struct tm tm;

    gmtime_r(t, &tm);
    strftime(buf, RFC1123_TIME_LEN+1, "---, %d --- %Y %H:%M:%S GMT", &tm);
    memcpy(buf, DAY_NAMES[tm.tm_wday], 3);
    memcpy(buf+8, MONTH_NAMES[tm.tm_mon], 3);

    return buf;
}

char * get_current_time(){
    time_t t;
    time(&t);
    return parse_rfc_datetime(&t);
}

void get_cli_argument(int argc, char **argv) {
    int ch;
    static struct option long_options[] = {
        {"port", required_argument, NULL, 'p'},
        {"root", required_argument, NULL, 'r'},
        {NULL, 0, NULL, 0}
    };
    while ((ch = getopt_long(argc, argv, "p:r:", long_options, NULL)) != -1) {
        switch (ch) {
            case 'p':
                printf("port: %s\n", optarg);
                strcpy(port, optarg);
                break;
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
    printf("file ext is: %s\n", file_ext);
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
        default:
            strcpy(status_message, "Bad Request");
    }
    char* currDate = get_current_time();
    sprintf(buf,
            "HTTP/1.1 %d %s\r\n"
            "Server: ICWS\r\n"
            "Connection: close\r\n"
            "Date: %s\r\n",
            status,
            status_message,
            currDate);
    free(currDate);
}

void response_error_template(int connFd, int status) {
    char responseBuf[MAXBUF];
    response_template(responseBuf, status);
    write_all(connFd, responseBuf, strlen(responseBuf));
}

void response_404(int connFd) {
    printf("Returning 404...\n");
    char buf[MAXBUF];
    char *msg = "<h1>404 Content not found</h1> ";
    response_template(buf, 404);
    sprintf(buf + strlen(buf),   
            "Content-Length: %lu\r\n"
            "Content-Type: text/html\r\n\r\n", strlen(msg));
    write_all(connFd, buf, strlen(buf));
    write_all(connFd, msg, strlen(msg));
}

void response_file(int inputFd, int connFd, int writeBody, char* mime_type) {
    char buf[MAXBUF];
    struct stat statBuffer;
    fstat(inputFd, &statBuffer);
    int fileSize = statBuffer.st_size;
    char * modified_time = parse_rfc_datetime(&statBuffer.st_mtime);
    response_template(buf, 200);
    sprintf(buf + strlen(buf), 
            "Content-Length: %d\r\n"
            "Content-Type: %s\r\n"
            "Last-Modified: %s\r\n\r\n", 
            fileSize, mime_type, modified_time);

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
    free(modified_time);
}

void get_file(char* filename, int connFd, int writeBody) {
    char *mime_type;
    char fullDir[MAXBUF];

    strcpy(fullDir, rootDir);
    strcat(fullDir, filename);
    printf("Full dir path: %s\n", fullDir);
    int inputFd = open(fullDir, O_RDONLY);
    if(inputFd < 0 || (mime_type=get_MIME(filename)) == NULL) {
        printf("File not found!\n");
        response_404(connFd);
    } else {
        response_file(inputFd, connFd, writeBody, mime_type);
        close(inputFd);
        free(mime_type);
    }
}

ssize_t get_request_buffer(int connFd, char* buf) {
    int totalRead = 0;
    int numRead;
    char eof_limiter[] = {'\r','\n'};
    int eof_pointer = 0;
    char *bufp = buf;
    printf("BUFFERING REQUEST....\n");
    while(totalRead <= MAX_HEADER_BUF) {
        // TODO:
        // Add nonblock read, and detect time elapsed for timeout request
        if((numRead = read(connFd, bufp, 1)) > 0) {
            totalRead += numRead;
            if(totalRead > MAX_HEADER_BUF) return -1;
            bufp += numRead;
            // When \r\n\r\n is send, terminate the sock stream;
            if(eof_limiter[eof_pointer % 2] == buf[totalRead-1]) {
                if(++eof_pointer == 4)
                    break;
            }
            else
                eof_pointer = 0;
        }
        else if(numRead == 0) {
            printf("Client terminate connection...\n");
            break;
        }
        else {
            printf("Ops error..\n");
            return -1;
        }
    }
    bufp = '\0';
    return totalRead;
}


void read_request(int connFd) {
    printf("Reading request\n");
    char buf[MAX_HEADER_BUF+555];
    // echo_logic(connFd);
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
            response_error_template(connFd, 501);
        }
        for(int index = 0;index < request->header_count; index++){
            printf("Request Header\n");
            printf("Header name %s Header Value %s\n",request->headers[index].header_name,request->headers[index].header_value);
        }
        free(request->headers);
        free(request);
    } else {
        printf("Bad request...\n");
        response_error_template(connFd, 400);
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
        printf("closing request..\n");
        close(connFd);
    }
}

int main(int argc, char **argv){
    //Read from the file the sample
    // https://stackoverflow.com/questions/7489093/getopt-long-proper-way-to-use-it
    get_cli_argument(argc, argv);
    start_server();
}




