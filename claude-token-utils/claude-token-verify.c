#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "crypto.h"

#define PORT 9123
#define LISTEN_ADDR "127.0.0.1"
#define BUF_SIZE 8192
#define STORAGE_PREFIX "storage-"
#define STORAGE_PREFIX_LEN 8

static unsigned char g_key[KEY_SIZE];

/* Extract Bearer token from HTTP request headers */
static char *extract_bearer_token(const char *request) {
    const char *pos = request;
    while (*pos) {
        if ((*pos == 'A' || *pos == 'a') &&
            strncasecmp(pos, "Authorization:", 14) == 0) {
            pos += 14;
            while (*pos == ' ' || *pos == '\t') pos++;
            if (strncasecmp(pos, "Bearer ", 7) == 0) {
                pos += 7;
                const char *end = pos;
                while (*end && *end != '\r' && *end != '\n') end++;
                size_t token_len = end - pos;
                char *token = malloc(token_len + 1);
                if (!token) return NULL;
                memcpy(token, pos, token_len);
                token[token_len] = '\0';
                return token;
            }
        }
        while (*pos && *pos != '\n') pos++;
        if (*pos == '\n') pos++;
    }
    return NULL;
}

static void send_response(int fd, int status, const char *status_text,
                           const char *body, const char *extra_headers) {
    char resp[4096];
    int body_len = body ? strlen(body) : 0;
    int n = snprintf(resp, sizeof(resp),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %d\r\n"
        "%s"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        status, status_text, body_len,
        extra_headers ? extra_headers : "",
        body ? body : "");
    write(fd, resp, n);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    if (load_key(KEY_PATH, g_key) != 0) {
        fprintf(stderr, "Failed to load encryption key from %s\n", KEY_PATH);
        return 1;
    }
    fprintf(stderr, "Encryption key loaded from %s\n", KEY_PATH);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
    };
    inet_pton(AF_INET, LISTEN_ADDR, &addr.sin_addr);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(server_fd); return 1;
    }
    if (listen(server_fd, 128) < 0) {
        perror("listen"); close(server_fd); return 1;
    }

    fprintf(stderr, "claude-token-verify listening on %s:%d\n", LISTEN_ADDR, PORT);

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        char buf[BUF_SIZE];
        ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
        if (n <= 0) { close(client_fd); continue; }
        buf[n] = '\0';

        char *token = extract_bearer_token(buf);

        if (!token) {
            send_response(client_fd, 401, "Unauthorized",
                "Unauthorized: missing or invalid Authorization header.", NULL);
            close(client_fd);
            continue;
        }

        /* Must start with storage- */
        if (strncmp(token, STORAGE_PREFIX, STORAGE_PREFIX_LEN) != 0) {
            fprintf(stderr, "DENY  token does not have storage- prefix\n");
            send_response(client_fd, 401, "Unauthorized",
                "Unauthorized: this API endpoint requires a storage- prefixed token; "
                "please contact an administrator for token.", NULL);
            free(token);
            close(client_fd);
            continue;
        }

        /* Decrypt the part after storage- */
        const char *encrypted_hex = token + STORAGE_PREFIX_LEN;
        char decrypted[MAX_TOKEN_LEN];

        if (decrypt_token(g_key, encrypted_hex, decrypted) != 0) {
            fprintf(stderr, "DENY  decryption failed for token\n");
            send_response(client_fd, 401, "Unauthorized",
                "Unauthorized: this token is not valid; "
                "please contact an administrator to request access.", NULL);
            free(token);
            close(client_fd);
            continue;
        }

        fprintf(stderr, "ALLOW decrypted token (len=%zu)\n", strlen(decrypted));

        /* Return 200 with the real token in X-Real-Authorization header */
        char header_buf[MAX_TOKEN_LEN + 64];
        snprintf(header_buf, sizeof(header_buf),
            "X-Real-Authorization: Bearer %s\r\n", decrypted);

        send_response(client_fd, 200, "OK", "OK", header_buf);
        free(token);
        close(client_fd);
    }

    close(server_fd);
    return 0;
}
