#ifndef __CGI_PARSE_HELPER_
#define __CGI_PARSE_HELPER_

struct environ_struct
{
   Request * request;
   std::string port;
   std::string server_name;
   std::string cgi_handler;
   int timeout;
};

int parse_cgi(environ_struct environ_vars, char *post_body, int connFd);

#endif