#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "render.h"
#include "httpserver.h"

#define DEFAULT_PORT 9124
#define LISTEN_ADDR "127.0.0.1"
#define BUF_SIZE 65536
#define PREFIX_DISPLAY_LEN 13  /* "storage-" (8) + 5 hex chars */
#define DEFAULT_LOG_PATH "/var/log/caddy/access.log"
#define RATELIMIT_LOG "/var/log/caddy/ratelimit.log"
#define DEFAULT_STATE_PATH "/var/lib/caddy/ratelimit-state.json"
#define SHANGHAI_OFFSET (8 * 3600)

static int g_port = DEFAULT_PORT;
static const char *g_log_path = DEFAULT_LOG_PATH;
static const char *g_state_path = DEFAULT_STATE_PATH;

token_state_t g_tokens[MAX_TOKENS];
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- Simple JSON field extraction ---- */

static int json_get_str(const char *json, const char *key, char *out, int maxlen) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '[') { p++; while (*p == ' ' || *p == '"') p++; }
    else if (*p == '"') { p++; }
    else return -1;
    int i = 0;
    while (*p && *p != '"' && i < maxlen - 1) out[i++] = *p++;
    out[i] = '\0';
    return 0;
}

static long json_get_long(const char *json, const char *key) {
    char buf[64];
    if (json_get_str(json, key, buf, sizeof(buf)) != 0) {
        char needle[128];
        snprintf(needle, sizeof(needle), "\"%s\":", key);
        const char *p = strstr(json, needle);
        if (!p) return -1;
        p += strlen(needle);
        while (*p == ' ') p++;
        if (*p == '[') { p++; while (*p == ' ' || *p == '"') p++; }
        return atol(p);
    }
    return atol(buf);
}

static double json_get_double(const char *json, const char *key) {
    char buf[64];
    if (json_get_str(json, key, buf, sizeof(buf)) == 0) return atof(buf);
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return -1.0;
    p += strlen(needle);
    while (*p == 0x20) p++;
    if (*p == 0x5b) { p++; while (*p == 0x20) p++; }
    return atof(p);
}

/* ---- Truncate prefix to "storage-xxxxx" format ---- */

static void truncate_prefix(char *prefix) {
    int len = strlen(prefix);
    while (len > 0 && prefix[len - 1] == '.') prefix[--len] = '\0';
    if (strncmp(prefix, "storage-", 8) == 0 && len > PREFIX_DISPLAY_LEN) {
        prefix[PREFIX_DISPLAY_LEN] = '\0';
    }
}

/* ---- Persistence ---- */

static void save_state(void) {
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", g_state_path);
    FILE *f = fopen(tmp_path, "w");
    if (!f) return;
    fprintf(f, "{\"tokens\":[\n");
    int first = 1;
    for (int i = 0; i < MAX_TOKENS; i++) {
        if (!g_tokens[i].active) continue;
        if (!first) fprintf(f, ",\n");
        first = 0;
        fprintf(f, "{\"prefix\":\"%s\",\"retry_after\":%ld,"
                "\"reset_ts\":%ld,\"util_5h\":%.4f,\"util_7d\":%.4f,"
                "\"status_5h\":\"%s\",\"status_7d\":\"%s\","
                "\"window\":\"%s\",\"last_seen\":%ld,\"banned\":%d}",
                g_tokens[i].prefix, g_tokens[i].retry_after,
                g_tokens[i].reset_ts, g_tokens[i].util_5h, g_tokens[i].util_7d,
                g_tokens[i].status_5h, g_tokens[i].status_7d,
                g_tokens[i].window, (long)g_tokens[i].last_seen,
                g_tokens[i].banned);
    }
    fprintf(f, "\n]}\n");
    fclose(f);
    rename(tmp_path, g_state_path);
}

static void load_state(void) {
    FILE *f = fopen(g_state_path, "r");
    if (!f) {
        fprintf(stderr, "No state file at %s, starting fresh\n", g_state_path);
        return;
    }
    char *buf = malloc(BUF_SIZE);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, BUF_SIZE - 1, f);
    buf[n] = '\0';
    fclose(f);

    int slot = 0;
    const char *p = buf;
    while (slot < MAX_TOKENS) {
        p = strstr(p, "{\"prefix\":");
        if (!p) break;
        const char *end = strchr(p + 1, '}');
        if (!end) break;
        int objlen = end - p + 1;
        char obj[1024];
        if (objlen >= (int)sizeof(obj)) { p = end + 1; continue; }
        memcpy(obj, p, objlen);
        obj[objlen] = '\0';

        token_state_t *t;
        char tmp_prefix[32];
        json_get_str(obj, "prefix", tmp_prefix, sizeof(tmp_prefix));
        truncate_prefix(tmp_prefix);

        time_t this_last_seen = (time_t)json_get_long(obj, "last_seen");
        int dup = -1;
        for (int d = 0; d < slot; d++) {
            if (g_tokens[d].active && strcmp(g_tokens[d].prefix, tmp_prefix) == 0) {
                dup = d;
                break;
            }
        }
        if (dup >= 0) {
            if (this_last_seen > g_tokens[dup].last_seen) {
                t = &g_tokens[dup];
            } else {
                p = end + 1;
                continue;
            }
        } else {
            t = &g_tokens[slot];
        }

        memset(t, 0, sizeof(*t));
        strncpy(t->prefix, tmp_prefix, sizeof(t->prefix) - 1);
        t->retry_after = json_get_long(obj, "retry_after");
        t->reset_ts = json_get_long(obj, "reset_ts");
        t->util_5h = json_get_double(obj, "util_5h");
        t->util_7d = json_get_double(obj, "util_7d");
        json_get_str(obj, "status_5h", t->status_5h, sizeof(t->status_5h));
        json_get_str(obj, "status_7d", t->status_7d, sizeof(t->status_7d));
        json_get_str(obj, "window", t->window, sizeof(t->window));
        long b = json_get_long(obj, "banned");
        t->banned = (b > 0) ? 1 : 0;
        t->last_seen = this_last_seen;
        t->active = 1;
        if (dup < 0) slot++;
        p = end + 1;
    }
    free(buf);
    fprintf(stderr, "Loaded %d token(s) from %s\n", slot, g_state_path);
}

/* ---- Token extraction and processing ---- */

static int extract_token_prefix(const char *json, char *out, int maxlen) {
    const char *p = strstr(json, "X-Debug-Token");
    if (!p) return -1;
    p = strstr(p, "Bearer storage-");
    if (!p) {
        p = strstr(json, "storage-");
        if (!p) return -1;
    } else {
        p += 7;
    }
    int i = 0;
    while (*p && *p != '"' && i < maxlen - 1 && i < PREFIX_DISPLAY_LEN)
        out[i++] = *p++;
    out[i] = '\0';
    return 0;
}

static token_state_t *find_or_alloc(const char *prefix) {
    int free_slot = -1;
    time_t oldest_time = 0;
    int oldest_slot = 0;
    for (int i = 0; i < MAX_TOKENS; i++) {
        if (g_tokens[i].active && strcmp(g_tokens[i].prefix, prefix) == 0)
            return &g_tokens[i];
        if (!g_tokens[i].active && free_slot < 0) free_slot = i;
        if (g_tokens[i].last_seen == 0 ||
            (oldest_time == 0 || g_tokens[i].last_seen < oldest_time)) {
            oldest_time = g_tokens[i].last_seen;
            oldest_slot = i;
        }
    }
    int slot = (free_slot >= 0) ? free_slot : oldest_slot;
    memset(&g_tokens[slot], 0, sizeof(token_state_t));
    strncpy(g_tokens[slot].prefix, prefix, sizeof(g_tokens[slot].prefix) - 1);
    g_tokens[slot].active = 1;
    return &g_tokens[slot];
}

static void process_line(const char *line) {
    if (!strstr(line, "X-Debug-Token")) return;

    char prefix[32];
    if (extract_token_prefix(line, prefix, sizeof(prefix)) != 0) return;

    long status = json_get_long(line, "status");
    int is_429 = (status == 429);
    int is_banned = (status == 400 || status == 401 || status == 403);

    double u5 = json_get_double(line, "Anthropic-Ratelimit-Unified-5h-Utilization");
    double u7 = json_get_double(line, "Anthropic-Ratelimit-Unified-7d-Utilization");
    if (u5 < 0 && u7 < 0 && !is_429 && !is_banned) return;

    pthread_mutex_lock(&g_lock);
    token_state_t *t = find_or_alloc(prefix);

    if (is_banned) t->banned = 1;
    else t->banned = 0;

    if (u5 >= 0) t->util_5h = u5;
    if (u7 >= 0) t->util_7d = u7;

    char tmp[16];
    if (json_get_str(line, "Anthropic-Ratelimit-Unified-5h-Status", tmp, sizeof(tmp)) == 0)
        strncpy(t->status_5h, tmp, sizeof(t->status_5h) - 1);
    if (json_get_str(line, "Anthropic-Ratelimit-Unified-7d-Status", tmp, sizeof(tmp)) == 0)
        strncpy(t->status_7d, tmp, sizeof(t->status_7d) - 1);
    if (json_get_str(line, "Anthropic-Ratelimit-Unified-Representative-Claim", tmp, sizeof(tmp)) == 0)
        strncpy(t->window, tmp, sizeof(t->window) - 1);

    if (is_429) {
        t->retry_after = json_get_long(line, "Retry-After");
        long reset = json_get_long(line, "Anthropic-Ratelimit-Unified-5h-Reset");
        if (reset <= 0) reset = json_get_long(line, "Anthropic-Ratelimit-Unified-Reset");
        if (reset > 0) t->reset_ts = reset;
    }

    if (strcmp(t->status_5h, "allowed") == 0 && strcmp(t->status_7d, "allowed") == 0) {
        t->reset_ts = 0;
        t->retry_after = 0;
    }

    t->last_seen = time(NULL);
    t->active = 1;
    save_state();
    pthread_mutex_unlock(&g_lock);

    if (is_429) {
        FILE *rl = fopen(RATELIMIT_LOG, "a");
        if (rl) {
            time_t now = time(NULL);
            struct tm *tm_p = gmtime(&now);
            char ts[64];
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_p);
            char reset_str[64] = "?";
            if (t->reset_ts > 0) {
                time_t rt = t->reset_ts + SHANGHAI_OFFSET;
                struct tm *rtm = gmtime(&rt);
                strftime(reset_str, sizeof(reset_str), "%H:%M:%S", rtm);
            }
            fprintf(rl, "[%s UTC] RATE_LIMITED token=%s retry_after=%lds "
                    "5h=[%s util=%.2f] 7d=[%s util=%.2f] resets_at=%s_Shanghai\n",
                    ts, t->prefix, t->retry_after,
                    t->status_5h, t->util_5h,
                    t->status_7d, t->util_7d, reset_str);
            fclose(rl);
        }
    }
}

/* ---- Log tailer thread ---- */

static void *log_tailer(void *arg) {
    (void)arg;
    FILE *f = NULL;
    long last_pos = 0;
    ino_t last_inode = 0;

    while (1) {
        if (!f) {
            f = fopen(g_log_path, "r");
            if (!f) { sleep(2); continue; }
            fseek(f, 0, SEEK_END);
            last_pos = ftell(f);
            struct stat st;
            if (fstat(fileno(f), &st) == 0) last_inode = st.st_ino;
        }
        struct stat st;
        if (stat(g_log_path, &st) != 0 || st.st_ino != last_inode) {
            fclose(f); f = NULL; last_pos = 0; continue;
        }
        long cur_size = st.st_size;
        if (cur_size < last_pos) { fseek(f, 0, SEEK_SET); last_pos = 0; }
        if (cur_size > last_pos) {
            fseek(f, last_pos, SEEK_SET);
            char line[BUF_SIZE];
            while (fgets(line, sizeof(line), f)) process_line(line);
            last_pos = ftell(f);
        }
        usleep(500000);
    }
    return NULL;
}

/* ---- Dashboard HTTP handler ---- */

char *g_response_buf;

void handle_dashboard(int client_fd, const char *request, int request_len) {
    (void)request_len;
    int len;
    if (strstr(request, "GET /api") || strstr(request, "GET /json"))
        len = render_json(g_response_buf, BUF_SIZE * 4);
    else
        len = render_html(g_response_buf, BUF_SIZE * 4);
    write(client_fd, g_response_buf, len);
}

int dashboard_init(const char *log_path, const char *state_path) {
    if (log_path) g_log_path = log_path;
    if (state_path) g_state_path = state_path;

    memset(g_tokens, 0, sizeof(g_tokens));
    load_state();

    pthread_t tailer_tid;
    if (pthread_create(&tailer_tid, NULL, log_tailer, NULL) != 0) {
        fprintf(stderr, "Failed to create log tailer thread\n");
        return -1;
    }
    pthread_detach(tailer_tid);
    fprintf(stderr, "Log tailer started for %s\n", g_log_path);

    g_response_buf = malloc(BUF_SIZE * 4);
    if (!g_response_buf) { perror("malloc"); return -1; }

    return 0;
}

int dashboard_main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) g_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) g_log_path = argv[++i];
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) g_state_path = argv[++i];
    }

    if (dashboard_init(NULL, NULL) != 0) return 1;

    int server_fd = http_server_create(LISTEN_ADDR, g_port);
    if (server_fd < 0) return 1;

    fprintf(stderr, "claude-api-gateway dashboard listening on %s:%d\n", LISTEN_ADDR, g_port);
    http_server_run(server_fd, handle_dashboard);

    free(g_response_buf);
    return 0;
}
