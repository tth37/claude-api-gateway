#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SERVICE_NAME "claude-api-gateway"
#define SERVICE_PATH "/etc/systemd/system/" SERVICE_NAME ".service"

static const char UNIT_FILE[] =
    "[Unit]\n"
    "Description=Claude API Gateway\n"
    "After=network.target\n"
    "\n"
    "[Service]\n"
    "Type=simple\n"
    "User=caddy\n"
    "Group=caddy\n"
    "ExecStart=/usr/local/bin/claude-api-gateway start server\n"
    "Restart=always\n"
    "RestartSec=3\n"
    "\n"
    "[Install]\n"
    "WantedBy=multi-user.target\n";

static int run(const char *cmd) {
    int ret = system(cmd);
    if (ret != 0) fprintf(stderr, "Command failed: %s\n", cmd);
    return ret;
}

static int do_install(void) {
    if (getuid() != 0) {
        fprintf(stderr, "Error: 'service install' must be run as root\n");
        return 1;
    }
    FILE *f = fopen(SERVICE_PATH, "w");
    if (!f) {
        perror("Failed to write service file");
        return 1;
    }
    fputs(UNIT_FILE, f);
    fclose(f);
    printf("Wrote %s\n", SERVICE_PATH);

    if (run("systemctl daemon-reload") != 0) return 1;
    if (run("systemctl enable " SERVICE_NAME) != 0) return 1;
    printf("Service installed and enabled.\n");
    printf("Run 'claude-api-gateway service start' to start it.\n");
    return 0;
}

static int do_uninstall(void) {
    if (getuid() != 0) {
        fprintf(stderr, "Error: 'service uninstall' must be run as root\n");
        return 1;
    }
    run("systemctl stop " SERVICE_NAME " 2>/dev/null");
    run("systemctl disable " SERVICE_NAME " 2>/dev/null");
    if (unlink(SERVICE_PATH) == 0)
        printf("Removed %s\n", SERVICE_PATH);
    else
        perror("Failed to remove service file");
    run("systemctl daemon-reload");
    printf("Service uninstalled.\n");
    return 0;
}

static int do_systemctl(const char *action) {
    if (getuid() != 0) {
        fprintf(stderr, "Error: 'service %s' must be run as root\n", action);
        return 1;
    }
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "systemctl %s " SERVICE_NAME, action);
    return run(cmd) == 0 ? 0 : 1;
}

int service_main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage: claude-api-gateway service <action>\n"
            "\n"
            "Actions:\n"
            "  install     Install and enable systemd service\n"
            "  uninstall   Stop, disable, and remove systemd service\n"
            "  start       Start the service\n"
            "  stop        Stop the service\n"
            "  restart     Restart the service\n"
            "  status      Show service status\n");
        return 1;
    }

    const char *action = argv[1];
    if (strcmp(action, "install") == 0) return do_install();
    if (strcmp(action, "uninstall") == 0) return do_uninstall();
    if (strcmp(action, "start") == 0) return do_systemctl("start");
    if (strcmp(action, "stop") == 0) return do_systemctl("stop");
    if (strcmp(action, "restart") == 0) return do_systemctl("restart");
    if (strcmp(action, "status") == 0) {
        system("systemctl status " SERVICE_NAME);
        return 0;
    }

    fprintf(stderr, "Unknown action: %s\n", action);
    return 1;
}
