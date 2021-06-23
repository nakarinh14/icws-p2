extern "C" {
    #include <stdio.h>
    #include <stdlib.h>
    #include <sys/types.h>
    #include <sys/stat.h>
    #include <sys/socket.h>
    #include <sys/poll.h>
    #include <netdb.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <getopt.h>
    #include <time.h>
    #include "pcsa_net.h"
    #include "parse.h"
}
#include <string>
#include <iostream> 
#include <vector>  
#include <thread>
#include <filesystem>
#include <pthread.h>
#include "simple_work_queue.hpp"
#include "cgi_helper.hpp"

namespace fs = std::filesystem;


#define MAX_HEADER_BUF 8192
#define MAXBUF 4096
#define READ_REQUEST_BUF 256
#define SERVER_NAME "ICWS"
using namespace std;

typedef struct sockaddr SA;

// Default parameter initialization
std::string cgiHandler;
std::string port = "8091";
fs::path rootDir("./");
int timeout = 5;
int numThreads = std::thread::hardware_concurrency();

std::vector<pthread_t> thread_pool;
struct {
    work_queue work_q;
} shared;
pthread_mutex_t parse_mutex;
pthread_cond_t queue_cond;

static const char *DAY_NAMES[] =
  { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *MONTH_NAMES[] =
  { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
const int RFC1123_TIME_LEN = 29;

// https://stackoverflow.com/questions/2726975/how-can-i-generate-an-rfc1123-date-string-from-c-code-win32
char * parse_rfc_datetime(time_t * t) {
    char * buf = (char *) malloc(RFC1123_TIME_LEN+1);
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
        {"numThreads", required_argument, NULL, 'n'},
        {"timeout", required_argument, NULL, 't'},
        {"cgiHandler", required_argument, NULL, 'c'},
        {NULL, 0, NULL, 0}
    };
    while ((ch = getopt_long(argc, argv, "p:r:n:t", long_options, NULL)) != -1) {
        switch (ch) {
            case 'p':
                printf("port: %s\n", optarg);
                port = optarg;
                break;
            case 'r':
                printf("rootdir: %s\n", optarg);
                rootDir = fs::path(optarg);
                std::cout << rootDir << std::endl;
                break;
            case 'n':
                printf("numThreads: %s\n", optarg);
                numThreads = atoi(optarg);
                break;
            case 't':
                printf("timeout: %s\n", optarg);
                timeout = atoi(optarg);
                break;
            case 'c':
                printf("cgi: %s\n", optarg);
                cgiHandler = optarg;
                std::cout << cgiHandler << std::endl;
                break;
        }
    }
}

std::string get_MIME(char * filename) {
    std::string tmp;
    const char *file_ext = strrchr(filename, '.');
    if(!file_ext || file_ext == filename) return tmp;
    // printf("file ext is: %s\n", file_ext);
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
        case 500:
            strcpy(status_message, "Internal Server Error");
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
            "Server: %s\r\n"
            "Connection: keep-alive\r\n"
            "Keep-Alive: timeout=%d, max=0\r\n"
            "Date: %s\r\n",
            status,
            status_message,
            SERVER_NAME,
            timeout,
            currDate);
    free(currDate);
}

void response_error_template(int connFd, int status) {
    char responseBuf[MAXBUF];
    char clrfBuf[] = "\r\n";
    response_template(responseBuf, status);
    write_all(connFd, responseBuf, strlen(responseBuf));
    write_all(connFd, clrfBuf, 2);
}

void response_404(int connFd) {
    // printf("Returning 404...\n");
    // fflush(stdout);
    char buf[MAXBUF];
    std::string msg = "<h1>404 Content not found</h1> ";
    response_template(buf, 404);
    sprintf(buf + strlen(buf),   
            "Content-Length: %lu\r\n"
            "Content-Type: text/html\r\n\r\n", strlen(msg.c_str()));
    write_all(connFd, buf, strlen(buf));
    write_all(connFd, &msg[0], strlen(msg.c_str()));
}

void response_file(int inputFd, int connFd, int writeBody, std::string mime_type) {
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
            fileSize, mime_type.c_str(), modified_time);

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
    std::string mime_type;
    fs::path filePath(filename);
    std::string fullDir = (rootDir / filePath).string();
    int inputFd = open(fullDir.c_str(), O_RDONLY);
    if (inputFd < 0 || (mime_type = get_MIME(filename)).empty()) {
        response_404(connFd);
    }
    else {
        response_file(inputFd, connFd, writeBody, mime_type);
    }
    if(inputFd) close(inputFd);
}
/* Finding the true request headers length, excluding any extra partial request or body */
int get_request_diff_crlf_length(char * buf) {
    char *searched = strstr(buf, "\r\n\r\n");
    if(searched == NULL) {
        return -1;
    }
    return strlen(buf) - strlen(searched+4);
}
/* Handle reading request from connFd to store in buffer */
ssize_t get_request_buffer(pollfd fds[], char* buf, int offset, int remain_content_length) {
    printf("Getting buffer\n");
    fflush(stdout);
    int numRead, rc, requestDiffLength;
    int connFd = fds[0].fd;
    int totalRead = offset;
    int maxLength = MAX_HEADER_BUF;
    if(remain_content_length > 0) {
        maxLength = remain_content_length;
    }
    char *bufp = buf + offset;

    while(totalRead < maxLength) {
        rc = poll(fds, 1, timeout * 1000);
        if (rc <= 0) break;
        if ((numRead = read(connFd, bufp, READ_REQUEST_BUF)) > 0) {
            totalRead += numRead;
            if(totalRead > maxLength) return -1;
            bufp += numRead;
            if (strlen(buf) >= 4) {
                requestDiffLength = get_request_diff_crlf_length(buf);
                if (requestDiffLength >= 0) {
                    totalRead = requestDiffLength;
                    break;
                }
            }
        }
        else {
            return -1;
        }
    }
    *bufp = '\0';
    return totalRead;
}

int get_content_length(Request * request) {
    int content_length = -1;
    for (int headerIndex = 0; headerIndex < request->header_count; headerIndex++){
        if(!strcmp(request->headers[headerIndex].header_name, "Content-Length")) {
            content_length = atoi(request->headers[headerIndex].header_value);
        }
    }
    return content_length;
}

/* Check if uri string is for cgi path */
int uri_is_cgi(char * uri) { 
    char cgiString[] = "/cgi/";
    int n = strlen(cgiString);
    if (strlen(uri) < n)
        return 0;

    for (int i = 0; i < n; i++) {
        if(uri[i] != cgiString[i]) return 0;
    }
    printf("URL IS CGI!!\n");
    fflush(stdout);
    return 1;
}

int buffer_partial_to_front(char *buf, int request_buf_len) {
    /* Shift partial request content to front for persistent pipelining */
    int partial_length = strlen(buf) - request_buf_len;
    if(partial_length > 0){
        memmove(buf, buf + request_buf_len, partial_length);
        memset(buf + partial_length, '\0', strlen(buf) - partial_length); // Set leftover to null terminating character
    }
    return partial_length;
}

void* thread_read_request(void *args) {
    for (;;) {
        char buf[MAX_HEADER_BUF + 555];
        int connFd, rc, request_partial_length = 0;
        // Wait for pthread_cond signal
        pthread_mutex_lock(&shared.work_q.jobs_mutex);
        while (shared.work_q.is_empty()) {
            pthread_cond_wait(&queue_cond, &shared.work_q.jobs_mutex);
        }
        shared.work_q.remove_job(&connFd);
        pthread_mutex_unlock(&shared.work_q.jobs_mutex);

        // Initialize polling for connFd
        struct pollfd fds[1];
        memset(fds, 0, sizeof(fds));
        fds[0].fd = connFd;
        fds[0].events = POLLIN;

        while((rc=poll(fds, 1, timeout * 1000)) > 0) {
            ssize_t request_buf_len = get_request_buffer(fds, buf, request_partial_length, -1);
            Request *request = NULL;
            if (request_buf_len > 0){
                pthread_mutex_lock(&parse_mutex);
                request = parse(buf, request_buf_len, connFd);
                pthread_mutex_unlock(&parse_mutex);
            }
            if(request == NULL || request_buf_len <= 0) {
                response_error_template(connFd, 400);
                break;
            }

            char *http_method = request->http_method;
            char *http_uri = request->http_uri;
            
            /* Handling CGI Request */
            if(uri_is_cgi(http_uri)) {
                char *post_body = NULL;
                if (!strcmp(http_method, "POST")){
                    // Continue fetching unfishied request.
                    int content_length = get_content_length(request);
                    if (content_length < 0 || request_buf_len + content_length > MAX_HEADER_BUF){
                        response_error_template(connFd, 500);
                        break;
                    }
                    int index_body_start = request_buf_len + 4;
                    get_request_buffer(fds, buf, strlen(buf), content_length - strlen(buf+index_body_start));
                    post_body = buf + index_body_start;
                }
                struct environ_struct environ_vars = {
                    request,
                    port,
                    std::string(SERVER_NAME),
                    cgiHandler,
                    timeout
                };
                if(parse_cgi(environ_vars, post_body, connFd) < 0) {
                    response_error_template(connFd, 500);
                }
                break;
            }
            /* Handling normal request */
            // Remove leading slash for filesystem path join to work properly
            while(strlen(http_uri) && http_uri[0] == '/')
                http_uri++;

            if(!strcmp(http_method, "GET")) 
                get_file(http_uri, connFd, 1);
            else if(!strcmp(http_method, "HEAD")) 
                get_file(http_uri, connFd, 0);
            else 
                response_error_template(connFd, 501);
                
            free(request->headers);
            free(request);
            request_partial_length = buffer_partial_to_front(buf, request_buf_len);
        }
        close(connFd);
    }
}

void initialize_thread_pools() {
    for (int i = 0; i < numThreads; i++){
        pthread_t new_thread;
        thread_pool.push_back(pthread_create(&new_thread, NULL, thread_read_request, (void *) NULL));
    }
}

int start_server() {
    int listenFd = open_listenfd(&port[0]);
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
        
        shared.work_q.add_job(connFd, &queue_cond);
    }
}

int main(int argc, char **argv){
    //Read from the file the sample
    // https://stackoverflow.com/questions/7489093/getopt-long-proper-way-to-use-it
    get_cli_argument(argc, argv);
    initialize_thread_pools();
    start_server();
}




