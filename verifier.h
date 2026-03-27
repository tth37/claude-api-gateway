#ifndef VERIFIER_H
#define VERIFIER_H

#include "crypto.h"

extern unsigned char g_key[KEY_SIZE];

char *extract_bearer_token(const char *request);
void handle_verify(int client_fd, const char *request, int request_len);
int verifier_main(int argc, char **argv);

#endif
