#include <stdio.h>
#include <string.h>

extern int encrypt_main(int argc, char **argv);
extern int verifier_main(int argc, char **argv);
extern int dashboard_main(int argc, char **argv);
extern int server_main(int argc, char **argv);

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <command> [args]\n"
        "\n"
        "Commands:\n"
        "  encrypt <raw-token>          Encrypt an API token\n"
        "  start server [-p port]       Start the combined server (verifier + dashboard)\n"
        "               [-l log] [-s state]\n"
        "  start verifier [-p port]     Start the token verifier only\n"
        "  start dashboard [-p port]    Start the dashboard only\n"
        "                  [-l log] [-s state]\n"
        "  help                         Show this help\n",
        prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2) { print_usage(argv[0]); return 1; }

    if (strcmp(argv[1], "encrypt") == 0)
        return encrypt_main(argc - 1, argv + 1);

    if (strcmp(argv[1], "start") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: 'start' requires a service name (verifier or dashboard)\n");
            print_usage(argv[0]);
            return 1;
        }
        if (strcmp(argv[2], "server") == 0)
            return server_main(argc - 2, argv + 2);
        if (strcmp(argv[2], "verifier") == 0)
            return verifier_main(argc - 2, argv + 2);
        if (strcmp(argv[2], "dashboard") == 0)
            return dashboard_main(argc - 2, argv + 2);
        fprintf(stderr, "Unknown service: %s\n", argv[2]);
        return 1;
    }

    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 ||
        strcmp(argv[1], "-h") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n", argv[1]);
    print_usage(argv[0]);
    return 1;
}
