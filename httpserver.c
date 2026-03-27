#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "httpserver.h"

int http_server_create(const char *addr, int port) {
    signal(SIGPIPE, SIG_IGN);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
    inet_pton(AF_INET, addr, &sa.sin_addr);

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, 128) < 0) {
        perror("listen"); close(fd); return -1;
    }
    return fd;
}

void http_server_run(int server_fd, http_handler_fn handler) {
    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        char buf[HTTP_BUF_SIZE];
        ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
        if (n <= 0) { close(client_fd); continue; }
        buf[n] = '\0';
        handler(client_fd, buf, (int)n);
        close(client_fd);
    }
}

void http_send_response(int fd, int status, const char *status_text,
                        const char *content_type, const char *body,
                        const char *extra_headers) {
    char resp[8192];
    int body_len = body ? strlen(body) : 0;
    int n = snprintf(resp, sizeof(resp),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "%s"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        status, status_text,
        content_type ? content_type : "text/plain",
        body_len,
        extra_headers ? extra_headers : "",
        body ? body : "");
    write(fd, resp, n);
}
