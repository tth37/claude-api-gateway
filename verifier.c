#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "crypto.h"
#include "httpserver.h"

#define DEFAULT_PORT 9123
#define LISTEN_ADDR "127.0.0.1"
#define STORAGE_PREFIX "storage-"
#define STORAGE_PREFIX_LEN 8

static unsigned char g_key[KEY_SIZE];

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

static void handle_verify(int client_fd, const char *request, int request_len) {
    (void)request_len;
    char *token = extract_bearer_token(request);

    if (!token) {
        http_send_response(client_fd, 401, "Unauthorized", "text/plain",
            "Unauthorized: missing or invalid Authorization header.", NULL);
        return;
    }

    if (strncmp(token, STORAGE_PREFIX, STORAGE_PREFIX_LEN) != 0) {
        fprintf(stderr, "DENY  token does not have storage- prefix\n");
        http_send_response(client_fd, 401, "Unauthorized", "text/plain",
            "Unauthorized: this API endpoint requires a storage- prefixed token; "
            "please contact an administrator for token.", NULL);
        free(token);
        return;
    }

    const char *encrypted_hex = token + STORAGE_PREFIX_LEN;
    char decrypted[MAX_TOKEN_LEN];

    if (decrypt_token(g_key, encrypted_hex, decrypted) != 0) {
        fprintf(stderr, "DENY  decryption failed for token\n");
        http_send_response(client_fd, 401, "Unauthorized", "text/plain",
            "Unauthorized: this token is not valid; "
            "please contact an administrator to request access.", NULL);
        free(token);
        return;
    }

    fprintf(stderr, "ALLOW decrypted token (len=%zu)\n", strlen(decrypted));

    char header_buf[MAX_TOKEN_LEN + 64];
    snprintf(header_buf, sizeof(header_buf),
        "X-Real-Authorization: Bearer %s\r\n", decrypted);

    http_send_response(client_fd, 200, "OK", "text/plain", "OK", header_buf);
    free(token);
}

int verifier_main(int argc, char **argv) {
    int port = DEFAULT_PORT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) port = atoi(argv[++i]);
    }

    if (load_key(KEY_PATH, g_key) != 0) {
        fprintf(stderr, "Failed to load encryption key from %s\n", KEY_PATH);
        return 1;
    }
    fprintf(stderr, "Encryption key loaded from %s\n", KEY_PATH);

    int server_fd = http_server_create(LISTEN_ADDR, port);
    if (server_fd < 0) return 1;

    fprintf(stderr, "claude-api-gateway verifier listening on %s:%d\n", LISTEN_ADDR, port);
    http_server_run(server_fd, handle_verify);
    return 0;
}
