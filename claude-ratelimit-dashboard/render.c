#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "render.h"

#define SHANGHAI_OFFSET (8 * 3600)

/* ---- Format a Shanghai time string ---- */

void fmt_shanghai(time_t ts, char *buf, int maxlen, const char *fmt) {
    time_t rt = ts + SHANGHAI_OFFSET;
    struct tm tm;
    gmtime_r(&rt, &tm);
    strftime(buf, maxlen, fmt, &tm);
}

/* ---- Sorting helper ---- */

static int cmp_last_seen_desc(const void *a, const void *b) {
    const token_state_t *ta = (const token_state_t *)a;
    const token_state_t *tb = (const token_state_t *)b;
    if (tb->last_seen > ta->last_seen) return 1;
    if (tb->last_seen < ta->last_seen) return -1;
    return 0;
}

/* ---- Display prefix: "storage-xxxxx" ---- */

static void display_prefix(const char *prefix, char *out, int maxlen) {
    /* prefix is like "storage-b03e7..." or "storage-b03e7" */
    if (strncmp(prefix, "storage-", 8) == 0 && strlen(prefix) >= 13) {
        snprintf(out, maxlen, "storage-%.5s", prefix + 8);
    } else {
        snprintf(out, maxlen, "%.*s", maxlen - 1, prefix);
    }
}

/* ---- HTML template parts ---- */

static const char HTML_HEAD[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Connection: close\r\n\r\n"
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    "<title>Rate Limit Dashboard</title>\n"
    "<script src=\"https://cdn.tailwindcss.com\"></script>\n"
    "<style>\n"
    "  @import url('https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600&family=JetBrains+Mono:wght@400;500&display=swap');\n"
    "  body { font-family: 'Inter', sans-serif; }\n"
    "  .mono { font-family: 'JetBrains Mono', monospace; }\n"
    "</style>\n"
    "</head>\n"
    "<body class=\"bg-gray-50 min-h-screen\">\n"
    "<div class=\"max-w-5xl mx-auto px-4 py-8 sm:px-6 lg:px-8\">\n"
    "  <div class=\"mb-8\">\n"
    "    <h1 class=\"text-2xl font-semibold text-gray-900\">Rate Limit Dashboard</h1>\n"
    "    <p class=\"mt-1 text-sm text-gray-500\">Claude API Gateway token status</p>\n"
    "  </div>\n";

static const char HTML_TABLE_START[] =
    "  <div class=\"bg-white rounded-xl shadow-sm border border-gray-200 overflow-hidden\">\n"
    "    <div class=\"overflow-x-auto\">\n"
    "      <table class=\"w-full\">\n"
    "        <thead>\n"
    "          <tr class=\"border-b border-gray-200 bg-gray-50/50\">\n"
    "            <th class=\"px-4 py-3 text-left text-xs font-medium text-gray-500 uppercase tracking-wider\">Token</th>\n"
    "            <th class=\"px-4 py-3 text-left text-xs font-medium text-gray-500 uppercase tracking-wider\">Status</th>\n"
    "            <th class=\"px-4 py-3 text-left text-xs font-medium text-gray-500 uppercase tracking-wider\">5h Usage</th>\n"
    "            <th class=\"px-4 py-3 text-left text-xs font-medium text-gray-500 uppercase tracking-wider\">7d Usage</th>\n"
    "            <th class=\"px-4 py-3 text-left text-xs font-medium text-gray-500 uppercase tracking-wider\">Resets At</th>\n"
    "            <th class=\"px-4 py-3 text-left text-xs font-medium text-gray-500 uppercase tracking-wider\">Countdown</th>\n"
    "            <th class=\"px-4 py-3 text-left text-xs font-medium text-gray-500 uppercase tracking-wider\">Last Seen</th>\n"
    "          </tr>\n"
    "        </thead>\n"
    "        <tbody class=\"divide-y divide-gray-100\">\n";

static const char HTML_EMPTY[] =
    "          <tr>\n"
    "            <td colspan=\"7\" class=\"px-4 py-12 text-center text-sm text-gray-400\">\n"
    "              No tokens tracked yet.\n"
    "            </td>\n"
    "          </tr>\n";

static const char HTML_TABLE_END[] =
    "        </tbody>\n"
    "      </table>\n"
    "    </div>\n"
    "  </div>\n";

static const char HTML_FOOT[] =
    "</div>\n"
    "</body>\n"
    "</html>\n";

/* ---- Util bar color class ---- */

static const char *util_color(double val) {
    if (val >= 1.0) return "bg-red-500";
    if (val >= 0.8) return "bg-amber-400";
    return "bg-emerald-500";
}

/* ---- HTML rendering ---- */

int render_html(char *buf, int maxlen) {
    time_t now = time(NULL);
    int n = 0;

    n += snprintf(buf + n, maxlen - n, "%s", HTML_HEAD);
    n += snprintf(buf + n, maxlen - n, "%s", HTML_TABLE_START);

    /* Collect and sort active tokens */
    token_state_t sorted[MAX_TOKENS];
    int count = 0;

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_TOKENS; i++) {
        if (!g_tokens[i].active) continue;
        memcpy(&sorted[count++], &g_tokens[i], sizeof(token_state_t));
    }
    pthread_mutex_unlock(&g_lock);

    qsort(sorted, count, sizeof(token_state_t), cmp_last_seen_desc);

    int has_footnote = 0;

    if (count == 0) {
        n += snprintf(buf + n, maxlen - n, "%s", HTML_EMPTY);
    }

    for (int i = 0; i < count; i++) {
        token_state_t *t = &sorted[i];

        int was_limited = (strcmp(t->status_5h, "rejected") == 0 ||
                           strcmp(t->status_7d, "rejected") == 0);
        int is_limited = was_limited;
        int newly_available = 0;

        /* If reset time has passed, treat as available */
        if (is_limited && t->reset_ts > 0 && now >= t->reset_ts) {
            is_limited = 0;
            newly_available = 1;
        }

        long remaining = 0;
        if (is_limited && t->reset_ts > 0 && now < t->reset_ts)
            remaining = t->reset_ts - now;
        int hours = remaining / 3600;
        int mins = (remaining % 3600) / 60;

        char reset_str[64] = "-";
        if (is_limited && t->reset_ts > 0)
            fmt_shanghai(t->reset_ts, reset_str, sizeof(reset_str), "%H:%M");

        double d5 = t->util_5h >= 0 ? t->util_5h : 0;
        double d7 = t->util_7d >= 0 ? t->util_7d : 0;
        int p5 = (int)(d5 * 100); if (p5 > 100) p5 = 100;
        int p7 = (int)(d7 * 100); if (p7 > 100) p7 = 100;

        char last_str[64] = "-";
        if (t->last_seen > 0)
            fmt_shanghai(t->last_seen, last_str, sizeof(last_str), "%m-%d %H:%M:%S");

        char countdown[32] = "-";
        if (is_limited && remaining > 0) {
            if (hours > 0)
                snprintf(countdown, sizeof(countdown), "%dh %dm", hours, mins);
            else
                snprintf(countdown, sizeof(countdown), "%dm", mins);
        }

        char display[32];
        display_prefix(t->prefix, display, sizeof(display));

        /* Status badge */
        const char *badge_class, *badge_text;
        if (is_limited) {
            badge_class = "bg-red-100 text-red-700 border-red-200";
            badge_text = "Rate Limited";
        } else if (newly_available) {
            badge_class = "bg-emerald-100 text-emerald-700 border-emerald-200";
            badge_text = "Available*";
            has_footnote = 1;
        } else {
            badge_class = "bg-emerald-100 text-emerald-700 border-emerald-200";
            badge_text = "Available";
        }

        n += snprintf(buf + n, maxlen - n,
            "          <tr class=\"hover:bg-gray-50/50 transition-colors\">\n"
            "            <td class=\"px-4 py-3\">"
              "<span class=\"mono text-sm font-medium text-gray-900\">%s</span>"
            "</td>\n"
            "            <td class=\"px-4 py-3\">"
              "<span class=\"inline-flex items-center px-2 py-0.5 rounded-md text-xs font-medium border %s\">%s</span>"
            "</td>\n",
            display, badge_class, badge_text);

        /* 5h usage */
        n += snprintf(buf + n, maxlen - n,
            "            <td class=\"px-4 py-3\">"
              "<div class=\"flex items-center gap-2\">"
              "<span class=\"text-sm text-gray-700 w-9 text-right\">%d%%</span>"
              "<div class=\"w-16 h-1.5 bg-gray-100 rounded-full overflow-hidden\">"
              "<div class=\"h-full rounded-full %s\" style=\"width:%d%%\"></div>"
              "</div></div></td>\n",
            (int)(d5 * 100), util_color(d5), p5);

        /* 7d usage */
        n += snprintf(buf + n, maxlen - n,
            "            <td class=\"px-4 py-3\">"
              "<div class=\"flex items-center gap-2\">"
              "<span class=\"text-sm text-gray-700 w-9 text-right\">%d%%</span>"
              "<div class=\"w-16 h-1.5 bg-gray-100 rounded-full overflow-hidden\">"
              "<div class=\"h-full rounded-full %s\" style=\"width:%d%%\"></div>"
              "</div></div></td>\n",
            (int)(d7 * 100), util_color(d7), p7);

        /* Resets At, Countdown, Last Seen */
        n += snprintf(buf + n, maxlen - n,
            "            <td class=\"px-4 py-3 text-sm text-gray-600\">%s</td>\n"
            "            <td class=\"px-4 py-3 text-sm text-gray-600\">%s</td>\n"
            "            <td class=\"px-4 py-3 text-xs text-gray-400 mono\">%s</td>\n"
            "          </tr>\n",
            reset_str, countdown, last_str);
    }

    n += snprintf(buf + n, maxlen - n, "%s", HTML_TABLE_END);

    if (has_footnote) {
        n += snprintf(buf + n, maxlen - n,
            "  <p class=\"mt-3 text-xs text-gray-400\">"
            "* Newly available &mdash; the rate limit reset time has passed but "
            "usage data may still show high utilization until the next API response updates it."
            "</p>\n");
    }

    n += snprintf(buf + n, maxlen - n, "%s", HTML_FOOT);
    return n;
}

/* ---- JSON API ---- */

int render_json(char *buf, int maxlen) {
    time_t now = time(NULL);
    int n = 0;
    n += snprintf(buf + n, maxlen - n,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n\r\n"
        "{\"tokens\":[");

    /* Collect and sort */
    token_state_t sorted[MAX_TOKENS];
    int count = 0;

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_TOKENS; i++) {
        if (!g_tokens[i].active) continue;
        memcpy(&sorted[count++], &g_tokens[i], sizeof(token_state_t));
    }
    pthread_mutex_unlock(&g_lock);

    qsort(sorted, count, sizeof(token_state_t), cmp_last_seen_desc);

    for (int i = 0; i < count; i++) {
        token_state_t *t = &sorted[i];
        int is_limited = (strcmp(t->status_5h, "rejected") == 0 ||
                          strcmp(t->status_7d, "rejected") == 0);
        int newly_available = 0;
        if (is_limited && t->reset_ts > 0 && now >= t->reset_ts) {
            is_limited = 0;
            newly_available = 1;
        }
        long remaining = 0;
        if (is_limited && t->reset_ts > 0 && now < t->reset_ts)
            remaining = t->reset_ts - now;

        char display[32];
        display_prefix(t->prefix, display, sizeof(display));

        if (i > 0) n += snprintf(buf + n, maxlen - n, ",");
        n += snprintf(buf + n, maxlen - n,
            "{\"prefix\":\"%s\",\"limited\":%s,"
            "\"newly_available\":%s,"
            "\"retry_after\":%ld,\"reset_ts\":%ld,"
            "\"util_5h\":%.2f,\"util_7d\":%.2f,"
            "\"status_5h\":\"%s\",\"status_7d\":\"%s\","
            "\"window\":\"%s\",\"last_seen\":%ld}",
            display,
            is_limited ? "true" : "false",
            newly_available ? "true" : "false",
            remaining, t->reset_ts,
            t->util_5h >= 0 ? t->util_5h : 0,
            t->util_7d >= 0 ? t->util_7d : 0,
            t->status_5h, t->status_7d,
            t->window, (long)t->last_seen);
    }
    n += snprintf(buf + n, maxlen - n, "]}");
    return n;
}
