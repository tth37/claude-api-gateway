#ifndef HTTPSERVER_H
#define HTTPSERVER_H

#define HTTP_BUF_SIZE 8192

typedef void (*http_handler_fn)(int client_fd, const char *request, int request_len);

/* Create, bind, listen on addr:port. Returns server_fd or -1. */
int http_server_create(const char *addr, int port);

/* Accept loop: blocks forever, calls handler for each request, closes client fd. */
void http_server_run(int server_fd, http_handler_fn handler);

/* Send an HTTP response with optional extra headers. */
void http_send_response(int fd, int status, const char *status_text,
                        const char *content_type, const char *body,
                        const char *extra_headers);

#endif
