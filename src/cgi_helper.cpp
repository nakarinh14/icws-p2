#define _GNU_SOURCE
extern "C" {
    #include <stdio.h>
    #include <unistd.h>
    #include <string.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <stdlib.h>
    #include "pcsa_net.h"
    #include "parse.h"
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
}

#include <unordered_map>
#include <iostream>
#include "cgi_helper.hpp"

#define BUFSIZE 2048

void fail_exit(char *msg) { 
    fprintf(stderr, "%s\n", msg); 
    exit(-1); 
}

std::unordered_map<std::string, std::string> header_env_name = {
    {"Accept", "HTTP_ACCEPT"},
    {"Host", "HTTP_HOST"},
    {"Cookie", "HTTP_COOKIE"},
    {"Referer", "HTTP_REFERER"},
    {"Accept-Encoding", "HTTP_ACCEPT_ENCODING"},
    {"Accept-Language", "HTTP_ACCEPT_LANGUAGE"},
    {"Accept-Charset", "HTTP_ACCEPT_CHARSET"},
    {"User-Agent", "HTTP_USER_AGENT"},
    {"Connection", "HTTP_CONNECTION"},
    {"Content-Length", "CONTENT_LENGTH"},
    {"Content-Type", "CONTENT_TYPE"}
};

void set_env(const char *key, const char *value, int *status) {
    if(setenv(key, value, 1) < 0) {
        *status = 0;
    }
}

int add_environ_vars(environ_struct environ_vars, int connFd) { 
    Request *request = environ_vars.request;
    int status = 1;
    set_env("REQUEST_METHOD", request->http_method, &status);
    set_env("REQUEST_URI", request->http_uri, &status);
    set_env("SCRIPT_NAME", environ_vars.cgi_handler.c_str(), &status);
    // set_env("QUERY_STRING", request->http_uri, &status);
    set_env("SERVER_PROTOCOL", "HTTP/1.1", &status);
    set_env("SERVER_PORT", environ_vars.port.c_str(), &status);
    set_env("SERVER_SOFTWARE", environ_vars.server_name.c_str(), &status);
    set_env("GATEWAY_INTERFACE", "CGI/1.1", &status);
    // Get IP addr from connFd
    struct sockaddr_in addr;
    socklen_t addr_size = sizeof(struct sockaddr_in);
    getpeername(connFd, (struct sockaddr *)&addr, &addr_size);
    set_env("REMOTE_ADDR", inet_ntoa(addr.sin_addr), &status);

    char tmpUri[strlen(request->http_uri)];
    strcpy(tmpUri, request->http_uri);
    
    char *queryPtr = strchr(tmpUri, '?');
    if(queryPtr != NULL) {    
        set_env("QUERY_STRING", queryPtr + 1, &status);
        *queryPtr = '\0';
    } else {
        set_env("QUERY_STRING", "", &status);
    }
    tmpUri[strlen(tmpUri) - 1] = '\0';
    set_env("PATH_INFO", tmpUri, &status);

    if(!status) {
        return -1;
    }

    int headerIndex;
    for (headerIndex = 0; headerIndex < request->header_count; headerIndex++) {
        char *headerName = request->headers[headerIndex].header_name;
        char *headerValue= request->headers[headerIndex].header_value;

        auto searched = header_env_name.find(std::string(headerName));
        if(searched != header_env_name.end()) {
            set_env(searched->second.c_str(), headerValue, &status);
            if (status < 0)
                return -1;
        }
    }
    return 1;
}

int parse_cgi(environ_struct environ_vars, char* post_body, int connFd) {
    int c2pFds[2]; /* Child to parent pipe */
    int p2cFds[2]; /* Parent to child pipe */

    if (pipe(c2pFds) < 0) return -1;
    if (pipe(p2cFds) < 0) return -1;
    
    int pid = fork();

    if (pid < 0) return -1;
    if (pid == 0) { /* Child - set up the conduit & run inferior cmd */

        /* Wire pipe's incoming to child's stdin */
        /* First, close the unused direction. */
        if (close(p2cFds[1]) < 0) fail_exit("failed to close p2c[1]");
        if (p2cFds[0] != STDIN_FILENO) {
            if (dup2(p2cFds[0], STDIN_FILENO) < 0)
                fail_exit("dup2 stdin failed.");
            if (close(p2cFds[0]) < 0)
                fail_exit("close p2c[0] failed.");
        }

        /* Wire child's stdout to pipe's outgoing */
        /* But first, close the unused direction */
        if (close(c2pFds[0]) < 0) fail_exit("failed to close c2p[0]");
        if (c2pFds[1] != STDOUT_FILENO) {
            if (dup2(c2pFds[1], STDOUT_FILENO) < 0)
                fail_exit("dup2 stdin failed.");
            if (close(c2pFds[1]) < 0)
                fail_exit("close pipeFd[0] failed.");
        }
        if(add_environ_vars(environ_vars, connFd) < 0) {
            return -1;
        };
        char *cgiCmd = &environ_vars.cgi_handler[0];
        char *inferiorArgv[] = {cgiCmd, NULL};
        if (execvpe(inferiorArgv[0], inferiorArgv, environ) < 0)
            fail_exit("exec failed.");
    }
    else { /* Parent - send a random message */
        /* Close the write direction in parent's incoming */
        if (close(c2pFds[1]) < 0) return -1;

        /* Close the read direction in parent's outgoing */
        if (close(p2cFds[0]) < 0) return -1;

        /* Write a post body to the child  */
        if(post_body != NULL) {
            write_all(p2cFds[1], post_body, strlen(post_body));
        }
        
        /* Close this end, done writing. */
        if (close(p2cFds[1]) < 0) return -1;

        char buf[BUFSIZE+1];
        ssize_t numRead;
        /* Begin reading from the child */
        while ((numRead = read(c2pFds[0], buf, BUFSIZE))>0) {
            write_all(connFd, buf, numRead);
            printf("Parent saw %ld bytes from child...\n", numRead);
            buf[numRead] = '\x0'; /* Printing hack; won't work with binary data */
            printf("-------\n");
            printf("%s", buf);
            printf("-------\n");
            fflush(stdout);
        }
        /* Close this end, done reading. */
        if (close(c2pFds[0]) < 0) return -1;

        /* Wait for child termination & reap */
        int status;

        if (waitpid(pid, &status, 0) < 0) return -1;
        printf("Child exited... parent's terminating as well.\n");
    }
    return 1;
}
