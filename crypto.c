#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include "crypto.h"

/* Convert a single hex char to its value (0-15), or -1 on error */
static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode hex string into bytes. Returns number of bytes, or -1 on error. */
static int hex_decode(const char *hex, unsigned char *out, int max_out) {
    int len = strlen(hex);
    if (len % 2 != 0) return -1;
    int n = len / 2;
    if (n > max_out) return -1;
    for (int i = 0; i < n; i++) {
        int hi = hex_val(hex[i * 2]);
        int lo = hex_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (hi << 4) | lo;
    }
    return n;
}

/* Encode bytes to hex string */
static void hex_encode(const unsigned char *in, int len, char *out) {
    for (int i = 0; i < len; i++)
        sprintf(out + i * 2, "%02x", in[i]);
    out[len * 2] = '\0';
}

int load_key(const char *path, unsigned char *key) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Cannot open key file: %s\n", path);
        return -1;
    }
    char hex[128];
    if (!fgets(hex, sizeof(hex), f)) {
        fclose(f);
        fprintf(stderr, "Cannot read key file\n");
        return -1;
    }
    fclose(f);

    /* Trim trailing whitespace */
    size_t slen = strlen(hex);
    while (slen > 0 && (hex[slen-1] == '\n' || hex[slen-1] == '\r' || hex[slen-1] == ' '))
        hex[--slen] = '\0';

    if (hex_decode(hex, key, KEY_SIZE) != KEY_SIZE) {
        fprintf(stderr, "Invalid key: expected %d hex chars\n", KEY_SIZE * 2);
        return -1;
    }
    return 0;
}

int encrypt_token(const unsigned char *key, const char *plaintext, char *output) {
    int pt_len = strlen(plaintext);
    unsigned char iv[IV_SIZE];
    unsigned char *ciphertext = malloc(pt_len + TAG_SIZE);
    unsigned char tag[TAG_SIZE];
    if (!ciphertext) return -1;

    /* Generate random IV */
    if (RAND_bytes(iv, IV_SIZE) != 1) {
        free(ciphertext);
        return -1;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { free(ciphertext); return -1; }

    int ret = -1;
    int len, ct_len;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto cleanup;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_SIZE, NULL) != 1) goto cleanup;
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv) != 1) goto cleanup;
    if (EVP_EncryptUpdate(ctx, ciphertext, &len, (unsigned char *)plaintext, pt_len) != 1) goto cleanup;
    ct_len = len;
    if (EVP_EncryptFinal_ex(ctx, ciphertext + len, &len) != 1) goto cleanup;
    ct_len += len;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_SIZE, tag) != 1) goto cleanup;

    /* Output = hex(iv || ciphertext || tag) */
    unsigned char *combined = malloc(IV_SIZE + ct_len + TAG_SIZE);
    if (!combined) goto cleanup;
    memcpy(combined, iv, IV_SIZE);
    memcpy(combined + IV_SIZE, ciphertext, ct_len);
    memcpy(combined + IV_SIZE + ct_len, tag, TAG_SIZE);
    hex_encode(combined, IV_SIZE + ct_len + TAG_SIZE, output);
    free(combined);
    ret = 0;

cleanup:
    EVP_CIPHER_CTX_free(ctx);
    free(ciphertext);
    return ret;
}

int decrypt_token(const unsigned char *key, const char *hex_input, char *output) {
    int hex_len = strlen(hex_input);
    int bin_len = hex_len / 2;

    /* Minimum: IV_SIZE + 1 byte ciphertext + TAG_SIZE */
    if (bin_len < IV_SIZE + TAG_SIZE + 1) return -1;

    unsigned char *bin = malloc(bin_len);
    if (!bin) return -1;
    if (hex_decode(hex_input, bin, bin_len) != bin_len) {
        free(bin);
        return -1;
    }

    unsigned char *iv = bin;
    int ct_len = bin_len - IV_SIZE - TAG_SIZE;
    unsigned char *ciphertext = bin + IV_SIZE;
    unsigned char *tag = bin + IV_SIZE + ct_len;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { free(bin); return -1; }

    int ret = -1;
    int len, pt_len;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto cleanup;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_SIZE, NULL) != 1) goto cleanup;
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv) != 1) goto cleanup;
    if (EVP_DecryptUpdate(ctx, (unsigned char *)output, &len, ciphertext, ct_len) != 1) goto cleanup;
    pt_len = len;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_SIZE, tag) != 1) goto cleanup;
    if (EVP_DecryptFinal_ex(ctx, (unsigned char *)output + len, &len) != 1) goto cleanup;
    pt_len += len;
    output[pt_len] = '\0';
    ret = 0;

cleanup:
    EVP_CIPHER_CTX_free(ctx);
    free(bin);
    return ret;
}
