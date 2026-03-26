#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>

#define DEFAULT_PORT 9124
#define LISTEN_ADDR "127.0.0.1"
#define BUF_SIZE 65536
#define MAX_TOKENS 64
#define TOKEN_PREFIX_LEN 20
#define DEFAULT_LOG_PATH "/var/log/caddy/access.log"
#define RATELIMIT_LOG "/var/log/caddy/ratelimit.log"
#define DEFAULT_STATE_PATH "/var/lib/caddy/ratelimit-state.json"
#define SHANGHAI_OFFSET (8 * 3600)

static int g_port = DEFAULT_PORT;
static const char *g_log_path = DEFAULT_LOG_PATH;
static const char *g_state_path = DEFAULT_STATE_PATH;

typedef struct {
    char prefix[32];
    int  active;
    long retry_after;
    long reset_ts;
    double util_5h;
    double util_7d;
    char status_5h[16];
    char status_7d[16];
    char window[16];
    time_t last_seen;
} token_state_t;

static token_state_t g_tokens[MAX_TOKENS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

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
                "\"window\":\"%s\",\"last_seen\":%ld}",
                g_tokens[i].prefix, g_tokens[i].retry_after,
                g_tokens[i].reset_ts, g_tokens[i].util_5h, g_tokens[i].util_7d,
                g_tokens[i].status_5h, g_tokens[i].status_7d,
                g_tokens[i].window, (long)g_tokens[i].last_seen);
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

        token_state_t *t = &g_tokens[slot];
        memset(t, 0, sizeof(*t));
        json_get_str(obj, "prefix", t->prefix, sizeof(t->prefix));
        t->retry_after = json_get_long(obj, "retry_after");
        t->reset_ts = json_get_long(obj, "reset_ts");
        t->util_5h = json_get_double(obj, "util_5h");
        t->util_7d = json_get_double(obj, "util_7d");
        json_get_str(obj, "status_5h", t->status_5h, sizeof(t->status_5h));
        json_get_str(obj, "status_7d", t->status_7d, sizeof(t->status_7d));
        json_get_str(obj, "window", t->window, sizeof(t->window));
        t->last_seen = (time_t)json_get_long(obj, "last_seen");
        t->active = 1;
        slot++;
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
    while (*p && *p != '"' && i < maxlen - 1 && i < (int)(8 + TOKEN_PREFIX_LEN))
        out[i++] = *p++;
    out[i] = '\0';
    if (i > 12) { out[12] = '.'; out[13] = '.'; out[14] = '.'; out[15] = '\0'; }
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
    /* Must have X-Debug-Token to identify which token this is */
    if (!strstr(line, "X-Debug-Token")) return;

    char prefix[32];
    if (extract_token_prefix(line, prefix, sizeof(prefix)) != 0) return;

    long status = json_get_long(line, "status");
    int is_429 = (status == 429);

    /* Check if rate limit headers are present */
    double u5 = json_get_double(line, "Anthropic-Ratelimit-Unified-5h-Utilization");
    double u7 = json_get_double(line, "Anthropic-Ratelimit-Unified-7d-Utilization");
    /* If no utilization headers at all, skip (e.g. auth errors) */
    if (u5 < 0 && u7 < 0 && !is_429) return;

    pthread_mutex_lock(&g_lock);
    token_state_t *t = find_or_alloc(prefix);

    /* Always update utilization and status from headers when present */
    if (u5 >= 0) t->util_5h = u5;
    if (u7 >= 0) t->util_7d = u7;

    char tmp[16];
    if (json_get_str(line, "Anthropic-Ratelimit-Unified-5h-Status", tmp, sizeof(tmp)) == 0)
        strncpy(t->status_5h, tmp, sizeof(t->status_5h) - 1);
    if (json_get_str(line, "Anthropic-Ratelimit-Unified-7d-Status", tmp, sizeof(tmp)) == 0)
        strncpy(t->status_7d, tmp, sizeof(t->status_7d) - 1);
    if (json_get_str(line, "Anthropic-Ratelimit-Unified-Representative-Claim", tmp, sizeof(tmp)) == 0)
        strncpy(t->window, tmp, sizeof(t->window) - 1);

    /* Only update retry/reset on 429 */
    if (is_429) {
        t->retry_after = json_get_long(line, "Retry-After");
        long reset = json_get_long(line, "Anthropic-Ratelimit-Unified-5h-Reset");
        if (reset <= 0) reset = json_get_long(line, "Anthropic-Ratelimit-Unified-Reset");
        if (reset > 0) t->reset_ts = reset;
    }

    /* If status changed to allowed and we had a reset_ts, clear it */
    if (strcmp(t->status_5h, "allowed") == 0 && strcmp(t->status_7d, "allowed") == 0) {
        t->reset_ts = 0;
        t->retry_after = 0;
    }

    t->last_seen = time(NULL);
    t->active = 1;
    save_state();
    pthread_mutex_unlock(&g_lock);

    /* Only log 429s to the ratelimit log */
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
    long last_inode = 0;

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

/* ---- Format a Shanghai time string ---- */
static void fmt_shanghai(time_t ts, char *buf, int maxlen, const char *fmt) {
    time_t rt = ts + SHANGHAI_OFFSET;
    struct tm tm;
    gmtime_r(&rt, &tm);
    strftime(buf, maxlen, fmt, &tm);
}

/* ---- HTML rendering ---- */

static int render_html(char *buf, int maxlen) {
    time_t now = time(NULL);
    int n = 0;

    n += snprintf(buf + n, maxlen - n,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<title>Rate Limit Status</title>"
        "<style>"
        "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
        "max-width:960px;margin:40px auto;padding:0 20px;background:#f5f5f5;color:#333}"
        "h1{font-size:1.4em;color:#555;border-bottom:2px solid #ddd;padding-bottom:10px}"
        "table{width:100%%;border-collapse:collapse;background:white;border-radius:8px;"
        "overflow:hidden;box-shadow:0 1px 3px rgba(0,0,0,0.1)}"
        "th{background:#667;color:white;padding:12px 16px;text-align:left;font-weight:500}"
        "td{padding:10px 16px;border-bottom:1px solid #eee}"
        "tr:last-child td{border-bottom:none}"
        ".limited{color:#c0392b;font-weight:600}"
        ".available{color:#27ae60;font-weight:600}"
        ".muted{color:#999;font-size:0.85em}"
        ".token{font-family:monospace;font-size:0.95em}"
        ".util-bar{width:80px;height:8px;background:#eee;border-radius:4px;display:inline-block;"
        "vertical-align:middle;margin-left:6px}"
        ".util-fill{height:100%%;border-radius:4px}"
        ".util-ok{background:#27ae60}"
        ".util-warn{background:#f39c12}"
        ".util-over{background:#c0392b}"
        ".empty{text-align:center;padding:40px;color:#999}"
        "</style></head><body>"
        "<h1>Claude API Rate Limit Status</h1>"
        "<table><tr>"
        "<th>Token</th><th>Status</th><th>5h Usage</th>"
        "<th>7d Usage</th><th>Resets At</th><th>Available In</th>"
        "<th>Last Updated</th>"
        "</tr>");

    pthread_mutex_lock(&g_lock);
    int count = 0;
    for (int i = 0; i < MAX_TOKENS; i++) {
        if (!g_tokens[i].active) continue;
        token_state_t *t = &g_tokens[i];

        int is_limited = (strcmp(t->status_5h, "rejected") == 0 ||
                          strcmp(t->status_7d, "rejected") == 0);
        /* If reset time has passed, treat as available */
        if (is_limited && t->reset_ts > 0 && now >= t->reset_ts)
            is_limited = 0;

        long remaining = 0;
        if (is_limited && t->reset_ts > 0 && now < t->reset_ts)
            remaining = t->reset_ts - now;
        int hours = remaining / 3600;
        int mins = (remaining % 3600) / 60;

        char reset_str[64] = "-";
        if (is_limited && t->reset_ts > 0)
            fmt_shanghai(t->reset_ts, reset_str, sizeof(reset_str), "%H:%M");

        double display_5h = t->util_5h >= 0 ? t->util_5h : 0;
        double display_7d = t->util_7d >= 0 ? t->util_7d : 0;

        const char *util5_class = display_5h >= 1.0 ? "util-over" :
                                  display_5h >= 0.8 ? "util-warn" : "util-ok";
        const char *util7_class = display_7d >= 1.0 ? "util-over" :
                                  display_7d >= 0.8 ? "util-warn" : "util-ok";
        int util5_pct = (int)(display_5h * 100);
        if (util5_pct > 100) util5_pct = 100;
        int util7_pct = (int)(display_7d * 100);
        if (util7_pct > 100) util7_pct = 100;

        char last_updated_str[64] = "-";
        if (t->last_seen > 0)
            fmt_shanghai(t->last_seen, last_updated_str, sizeof(last_updated_str),
                         "%m-%d %H:%M:%S");

        char countdown[32] = "-";
        if (is_limited && remaining > 0) {
            if (hours > 0)
                snprintf(countdown, sizeof(countdown), "%dh %dm", hours, mins);
            else
                snprintf(countdown, sizeof(countdown), "%dm", mins);
        }

        n += snprintf(buf + n, maxlen - n,
            "<tr>"
            "<td class='token'>%s</td>"
            "<td class='%s'>%s</td>"
            "<td>%d%%<div class='util-bar'><div class='util-fill %s' "
            "style='width:%d%%'></div></div></td>"
            "<td>%d%%<div class='util-bar'><div class='util-fill %s' "
            "style='width:%d%%'></div></div></td>"
            "<td>%s</td>"
            "<td>%s</td>"
            "<td class='muted'>%s</td>"
            "</tr>",
            t->prefix,
            is_limited ? "limited" : "available",
            is_limited ? "RATE LIMITED" : "Available",
            (int)(display_5h * 100), util5_class, util5_pct,
            (int)(display_7d * 100), util7_class, util7_pct,
            reset_str,
            countdown,
            last_updated_str);
        count++;
    }
    pthread_mutex_unlock(&g_lock);

    if (count == 0) {
        n += snprintf(buf + n, maxlen - n,
            "<tr><td colspan='7' class='empty'>"
            "No tokens tracked yet.</td></tr>");
    }

    n += snprintf(buf + n, maxlen - n, "</table></body></html>");
    return n;
}

/* ---- JSON API ---- */

static int render_json(char *buf, int maxlen) {
    time_t now = time(NULL);
    int n = 0;
    n += snprintf(buf + n, maxlen - n,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n\r\n"
        "{\"tokens\":[");

    pthread_mutex_lock(&g_lock);
    int first = 1;
    for (int i = 0; i < MAX_TOKENS; i++) {
        if (!g_tokens[i].active) continue;
        token_state_t *t = &g_tokens[i];
        int is_limited = (strcmp(t->status_5h, "rejected") == 0 ||
                          strcmp(t->status_7d, "rejected") == 0);
        /* If reset time has passed, treat as available */
        if (is_limited && t->reset_ts > 0 && now >= t->reset_ts)
            is_limited = 0;
        long remaining = 0;
        if (is_limited && t->reset_ts > 0 && now < t->reset_ts)
            remaining = t->reset_ts - now;
        if (!first) n += snprintf(buf + n, maxlen - n, ",");
        first = 0;
        n += snprintf(buf + n, maxlen - n,
            "{\"prefix\":\"%s\",\"limited\":%s,"
            "\"retry_after\":%ld,\"reset_ts\":%ld,"
            "\"util_5h\":%.2f,\"util_7d\":%.2f,"
            "\"status_5h\":\"%s\",\"status_7d\":\"%s\","
            "\"window\":\"%s\",\"last_seen\":%ld}",
            t->prefix,
            is_limited ? "true" : "false",
            remaining, t->reset_ts,
            t->util_5h >= 0 ? t->util_5h : 0,
            t->util_7d >= 0 ? t->util_7d : 0,
            t->status_5h, t->status_7d,
            t->window, (long)t->last_seen);
    }
    pthread_mutex_unlock(&g_lock);
    n += snprintf(buf + n, maxlen - n, "]}");
    return n;
}

/* ---- HTTP server ---- */

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);
    memset(g_tokens, 0, sizeof(g_tokens));

    /* Parse args: [-p port] [-l log_path] [-s state_path] */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) g_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) g_log_path = argv[++i];
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) g_state_path = argv[++i];
    }

    load_state();

    pthread_t tailer_tid;
    if (pthread_create(&tailer_tid, NULL, log_tailer, NULL) != 0) {
        fprintf(stderr, "Failed to create log tailer thread\n");
        return 1;
    }
    pthread_detach(tailer_tid);
    fprintf(stderr, "Log tailer started for %s\n", g_log_path);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(g_port) };
    inet_pton(AF_INET, LISTEN_ADDR, &addr.sin_addr);
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(server_fd, 64) < 0) { perror("listen"); return 1; }
    fprintf(stderr, "claude-ratelimit-dashboard listening on %s:%d\n", LISTEN_ADDR, g_port);

    char *response_buf = malloc(BUF_SIZE * 4);
    if (!response_buf) { perror("malloc"); return 1; }

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) { if (errno == EINTR) continue; perror("accept"); continue; }

        char req[4096];
        ssize_t nr = read(client_fd, req, sizeof(req) - 1);
        if (nr <= 0) { close(client_fd); continue; }
        req[nr] = '\0';

        int len;
        if (strstr(req, "GET /api") || strstr(req, "GET /json"))
            len = render_json(response_buf, BUF_SIZE * 4);
        else
            len = render_html(response_buf, BUF_SIZE * 4);

        write(client_fd, response_buf, len);
        close(client_fd);
    }

    free(response_buf);
    close(server_fd);
    return 0;
}
