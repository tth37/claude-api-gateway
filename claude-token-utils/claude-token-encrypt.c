#include <stdio.h>
#include <string.h>
#include "crypto.h"

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <raw-api-token>\n", argv[0]);
        fprintf(stderr, "Encrypts a raw Anthropic API token and outputs a storage- prefixed token.\n");
        return 1;
    }

    unsigned char key[KEY_SIZE];
    if (load_key(KEY_PATH, key) != 0)
        return 1;

    /* output: hex of (iv + ciphertext + tag), max ~2x input + overhead */
    char output[MAX_TOKEN_LEN * 2 + (IV_SIZE + TAG_SIZE) * 2 + 1];
    if (encrypt_token(key, argv[1], output) != 0) {
        fprintf(stderr, "Encryption failed\n");
        return 1;
    }

    printf("storage-%s\n", output);
    return 0;
}
