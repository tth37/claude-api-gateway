#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/rand.h>
#include "crypto.h"

#define CONFIG_DIR "/etc/claude-api-gateway"

int setup_main(int argc, char **argv) {
    (void)argc; (void)argv;

    if (getuid() != 0) {
        fprintf(stderr, "Error: 'setup' must be run as root\n");
        return 1;
    }

    /* Create config directory */
    struct stat st;
    if (stat(CONFIG_DIR, &st) != 0) {
        if (mkdir(CONFIG_DIR, 0755) != 0) {
            perror("Failed to create " CONFIG_DIR);
            return 1;
        }
        printf("Created %s\n", CONFIG_DIR);
    }

    /* Check if key already exists */
    if (access(KEY_PATH, F_OK) == 0) {
        printf("Encryption key already exists at %s, skipping\n", KEY_PATH);
        return 0;
    }

    /* Generate random 256-bit key */
    unsigned char key[KEY_SIZE];
    if (RAND_bytes(key, KEY_SIZE) != 1) {
        fprintf(stderr, "Failed to generate random key\n");
        return 1;
    }

    /* Write hex-encoded key */
    FILE *f = fopen(KEY_PATH, "w");
    if (!f) {
        perror("Failed to write key file");
        return 1;
    }
    for (int i = 0; i < KEY_SIZE; i++)
        fprintf(f, "%02x", key[i]);
    fprintf(f, "\n");
    fclose(f);

    /* Restrict permissions */
    if (chmod(KEY_PATH, 0600) != 0)
        perror("Warning: failed to chmod key file");

    printf("Generated encryption key at %s\n", KEY_PATH);
    return 0;
}
