#ifndef DASHBOARD_H
#define DASHBOARD_H

int  dashboard_init(const char *log_path, const char *state_path);
void handle_dashboard(int client_fd, const char *request, int request_len);
int  dashboard_main(int argc, char **argv);

#endif
