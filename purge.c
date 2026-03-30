#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_STATE_PATH "/var/lib/caddy/ratelimit-state.json"
#define BUF_SIZE (1024 * 1024)

static int json_get_str(const char *json, const char *key, char *out, int maxlen) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *p = strstr(json, needle);
    if (!p) { out[0] = '\0'; return -1; }
    p += strlen(needle);
    const char *end = strchr(p, '"');
    if (!end) { out[0] = '\0'; return -1; }
    int len = end - p;
    if (len >= maxlen) len = maxlen - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

static long json_get_long(const char *json, const char *key) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    return atol(p);
}

static double json_get_double(const char *json, const char *key) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    return atof(p);
}

typedef struct {
    char prefix[32];
    long retry_after, reset_ts;
    double util_5h, util_7d;
    char status_5h[16], status_7d[16], window[16];
    long last_seen;
    int banned, active;
} entry_t;

int purge_main(int argc, char **argv) {
    const char *state_path = DEFAULT_STATE_PATH;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            state_path = argv[++i];
    }

    FILE *f = fopen(state_path, "r");
    if (!f) {
        fprintf(stderr, "Cannot open state file: %s\n", state_path);
        return 1;
    }
    char *buf = malloc(BUF_SIZE);
    if (!buf) { fclose(f); return 1; }
    size_t n = fread(buf, 1, BUF_SIZE - 1, f);
    buf[n] = '\0';
    fclose(f);

    /* Parse tokens */
    entry_t entries[64];
    int count = 0, removed = 0;
    const char *p = buf;
    while (count < 64) {
        p = strstr(p, "{\"prefix\":");
        if (!p) break;
        const char *end = strchr(p + 1, '}');
        if (!end) break;
        int objlen = end - p + 1;
        char obj[1024];
        if (objlen >= (int)sizeof(obj)) { p = end + 1; continue; }
        memcpy(obj, p, objlen);
        obj[objlen] = '\0';

        entry_t *e = &entries[count];
        memset(e, 0, sizeof(*e));
        json_get_str(obj, "prefix", e->prefix, sizeof(e->prefix));
        e->retry_after = json_get_long(obj, "retry_after");
        e->reset_ts = json_get_long(obj, "reset_ts");
        e->util_5h = json_get_double(obj, "util_5h");
        e->util_7d = json_get_double(obj, "util_7d");
        json_get_str(obj, "status_5h", e->status_5h, sizeof(e->status_5h));
        json_get_str(obj, "status_7d", e->status_7d, sizeof(e->status_7d));
        json_get_str(obj, "window", e->window, sizeof(e->window));
        e->last_seen = json_get_long(obj, "last_seen");
        long b = json_get_long(obj, "banned");
        e->banned = (b > 0) ? 1 : 0;
        e->active = 1;
        count++;
        p = end + 1;
    }
    free(buf);

    /* Remove banned tokens */
    for (int i = 0; i < count; i++) {
        if (entries[i].banned) {
            printf("Removing banned token: %s\n", entries[i].prefix);
            entries[i].active = 0;
            removed++;
        }
    }

    if (removed == 0) {
        printf("No banned tokens found.\n");
        return 0;
    }

    /* Write back */
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", state_path);
    f = fopen(tmp_path, "w");
    if (!f) {
        perror("Failed to write state file");
        return 1;
    }
    fprintf(f, "{\"tokens\":[\n");
    int first = 1;
    for (int i = 0; i < count; i++) {
        if (!entries[i].active) continue;
        if (!first) fprintf(f, ",\n");
        first = 0;
        fprintf(f, "{\"prefix\":\"%s\",\"retry_after\":%ld,"
                "\"reset_ts\":%ld,\"util_5h\":%.4f,\"util_7d\":%.4f,"
                "\"status_5h\":\"%s\",\"status_7d\":\"%s\","
                "\"window\":\"%s\",\"last_seen\":%ld,\"banned\":%d}",
                entries[i].prefix, entries[i].retry_after,
                entries[i].reset_ts, entries[i].util_5h, entries[i].util_7d,
                entries[i].status_5h, entries[i].status_7d,
                entries[i].window, entries[i].last_seen, entries[i].banned);
    }
    fprintf(f, "\n]}\n");
    fclose(f);
    rename(tmp_path, state_path);

    printf("Removed %d banned token(s). Restart the service to apply:\n", removed);
    printf("  claude-api-gateway service restart\n");
    return 0;
}
