#ifndef CRYPTO_H
#define CRYPTO_H

#define KEY_SIZE 32       /* AES-256 */
#define IV_SIZE 12        /* GCM standard */
#define TAG_SIZE 16       /* GCM tag */
#define KEY_PATH "/etc/claude-api-gateway/encryption.key"
#define MAX_TOKEN_LEN 512

/* Load 256-bit key from hex file. Returns 0 on success, -1 on error. */
int load_key(const char *path, unsigned char *key);

/*
 * Encrypt plaintext token using AES-256-GCM.
 * Output is hex-encoded string of (iv || ciphertext || tag).
 * output buffer must be at least (IV_SIZE + plaintext_len + TAG_SIZE) * 2 + 1.
 * Returns 0 on success, -1 on error.
 */
int encrypt_token(const unsigned char *key, const char *plaintext, char *output);

/*
 * Decrypt hex-encoded (iv || ciphertext || tag) using AES-256-GCM.
 * output buffer must be at least MAX_TOKEN_LEN.
 * Returns 0 on success, -1 on error (invalid ciphertext or auth failure).
 */
int decrypt_token(const unsigned char *key, const char *hex_input, char *output);

#endif
