#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "httpserver.h"
#include "verifier.h"
#include "dashboard.h"

#define DEFAULT_PORT 9123
#define LISTEN_ADDR "127.0.0.1"

static void handle_request(int client_fd, const char *request, int request_len) {
    if (strstr(request, "GET /verify") || strstr(request, "POST /verify"))
        handle_verify(client_fd, request, request_len);
    else
        handle_dashboard(client_fd, request, request_len);
}

int server_main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    const char *log_path = NULL;
    const char *state_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) log_path = argv[++i];
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) state_path = argv[++i];
    }

    if (load_key(KEY_PATH, g_key) != 0) {
        fprintf(stderr, "Failed to load encryption key from %s\n", KEY_PATH);
        return 1;
    }
    fprintf(stderr, "Encryption key loaded from %s\n", KEY_PATH);

    if (dashboard_init(log_path, state_path) != 0) return 1;

    int server_fd = http_server_create(LISTEN_ADDR, port);
    if (server_fd < 0) return 1;

    fprintf(stderr, "claude-api-gateway server listening on %s:%d\n", LISTEN_ADDR, port);
    http_server_run(server_fd, handle_request);
    return 0;
}
