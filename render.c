#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "render.h"

#define SHANGHAI_OFFSET (8 * 3600)

/* ---- Format a Shanghai time string (used by ratelimit.log only) ---- */

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
    "<title>Claude API Gateway</title>\n"
    "<link rel=\"icon\" type=\"image/svg+xml\" href=\"data:image/svg+xml;base64,PHN2ZyB3aWR0aD0iMjQ4IiBoZWlnaHQ9IjI0OCIgdmlld0JveD0iMCAwIDI0OCAyNDgiIGZpbGw9Im5vbmUiIHhtbG5zPSJodHRwOi8vd3d3LnczLm9yZy8yMDAwL3N2ZyI+CjxwYXRoIGQ9Ik01Mi40Mjg1IDE2Mi44NzNMOTguNzg0NCAxMzYuODc5TDk5LjU0ODUgMTM0LjYwMkw5OC43ODQ0IDEzMy4zMzRIOTYuNDkyMUw4OC43MjM3IDEzMi44NjJMNjIuMjM0NiAxMzIuMTUzTDM5LjMxMTMgMTMxLjIwN0wxNy4wMjQ5IDEzMC4wMjZMMTEuNDIxNCAxMjguODQ0TDYuMiAxMjEuODczTDYuNzA5NCAxMTguNDQ3TDExLjQyMTQgMTE1LjI1N0wxOC4xNzEgMTE1Ljg0N0wzMy4wNzExIDExNi45MTFMNTUuNDg1IDExOC40NDdMNzEuNjU4NiAxMTkuMzkyTDk1LjcyOCAxMjEuODczSDk5LjU0ODVMMTAwLjA1OCAxMjAuMzM3TDk4Ljc4NDQgMTE5LjM5Mkw5Ny43NjU2IDExOC40NDdMNzQuNTg3NyAxMDIuNzMyTDQ5LjQ5OTUgODYuMTkwNUwzNi4zODIzIDc2LjYyTDI5LjM3NzkgNzEuNzc1N0wyNS44MTIxIDY3LjI4NThMMjQuMjgzOSA1Ny4zNjA4TDMwLjY1MTUgNTAuMjcxNkwzOS4zMTEzIDUwLjg2MjNMNDEuNDc2MyA1MS40NTMxTDUwLjI2MzYgNTguMTg3OUw2OC45ODQyIDcyLjcyMDlMOTMuNDM1NyA5MC42ODA0TDk3LjAwMTUgOTMuNjM0M0w5OC40Mzc0IDkyLjY2NTJMOTguNjU3MSA5MS45ODAxTDk3LjAwMTUgODkuMjYyNUw4My43NTcgNjUuMjc3Mkw2OS42MjEgNDAuODE5Mkw2My4yNTM0IDMwLjY1NzlMNjEuNTk3OCAyNC42MzJDNjAuOTU2NSAyMi4xMDMyIDYwLjU3OSAyMC4wMTExIDYwLjU3OSAxNy40MjQ2TDY3LjgzODEgNy40OTk2NUw3MS45MTMzIDYuMTk5OTVMODEuNzE5MyA3LjQ5OTY1TDg1Ljc5NDYgMTEuMDQ0M0w5MS45MDc0IDI0Ljk4NjVMMTAxLjcxNCA0Ni44NDUxTDExNi45OTYgNzYuNjJMMTIxLjQ1MyA4NS40ODE2TDEyMy44NzMgOTMuNjM0M0wxMjQuNzY0IDk2LjExNTVIMTI2LjI5MlY5NC42OTc2TDEyNy41NjYgNzcuOTE5N0wxMjkuODU4IDU3LjM2MDhMMTMyLjE1IDMwLjg5NDJMMTMyLjkxNSAyMy40NTA1TDEzNi42MDggMTQuNDcwOEwxNDMuOTk0IDkuNjI2NDNMMTQ5LjcyNSAxMi4zNDRMMTU0LjQzNyAxOS4wNzg4TDE1My44IDIzLjQ1MDVMMTUwLjk5OCA0MS42NDYzTDE0NS41MjIgNzAuMTIxNUwxNDEuOTU3IDg5LjI2MjVIMTQzLjk5NEwxNDYuNDE0IDg2Ljc4MTNMMTU2LjA5MyA3NC4wMjA2TDE3Mi4yNjYgNTMuNjk4TDE3OS4zOTggNDUuNjYzNUwxODcuODAzIDM2LjgwMkwxOTMuMTUyIDMyLjU0ODRIMjAzLjM0TDIxMC43MjYgNDMuNjU0OUwyMDcuNDE1IDU1LjExNTlMMTk2Ljk3MiA2OC4zNDkyTDE4OC4zMTIgNzkuNTczOUwxNzUuODk2IDk2LjIwOTVMMTY4LjE5MSAxMDkuNTg1TDE2OC44ODIgMTEwLjY4OUwxNzAuNzM4IDExMC41M0wxOTguNzU1IDEwNC41MDRMMjEzLjkxIDEwMS43ODdMMjMxLjk5NCA5OC43MTQ5TDI0MC4xNDQgMTAyLjQ5NkwyNDEuMDM2IDEwNi4zOTVMMjM3Ljg1MiAxMTQuMzExTDIxOC40OTUgMTE5LjAzN0wxOTUuODI2IDEyMy42NDVMMTYyLjA3IDEzMS41OTJMMTYxLjY5NiAxMzEuODkzTDE2Mi4xMzcgMTMyLjU0N0wxNzcuMzYgMTMzLjkyNUwxODMuODU1IDEzNC4yNzlIMTk5Ljc3NEwyMjkuNDQ3IDEzNi41MjRMMjM3LjIxNSAxNDEuNjA1TDI0MS44IDE0Ny44NjdMMjQxLjAzNiAxNTIuNzExTDIyOS4wNjUgMTU4LjczN0wyMTMuMDE5IDE1NC45NTZMMTc1LjQ1IDE0NS45NzdMMTYyLjU4NyAxNDIuNzg3SDE2MC44MDVWMTQzLjg1TDE3MS41MDIgMTU0LjM2NkwxOTEuMjQyIDE3Mi4wODlMMjE1LjgyIDE5NS4wMTFMMjE3LjA5NCAyMDAuNjgyTDIxMy45MSAyMDUuMTcyTDIxMC41OTkgMjA0LjY5OUwxODguOTQ5IDE4OC4zOTRMMTgwLjU0NCAxODEuMDY5TDE2MS42OTYgMTY1LjExOEgxNjAuNDIyVjE2Ni43NzJMMTY0Ljc1MiAxNzMuMTUyTDE4Ny44MDMgMjA3Ljc3MUwxODguOTQ5IDIxOC40MDVMMTg3LjI5NCAyMjEuODMyTDE4MS4zMDggMjIzLjk1OUwxNzQuODEzIDIyMi43NzdMMTYxLjE4NyAyMDMuNzU0TDE0Ny4zMDUgMTgyLjQ4NkwxMzYuMDk4IDE2My4zNDVMMTM0Ljc0NSAxNjQuMkwxMjguMDc1IDIzNS40MkwxMjUuMDE5IDIzOS4wODJMMTE3Ljg4NyAyNDEuOEwxMTEuOTAyIDIzNy4zMUwxMDguNzE4IDIyOS45ODRMMTExLjkwMiAyMTUuNDUyTDExNS43MjIgMTk2LjU0N0wxMTguNzc5IDE4MS41NDFMMTIxLjU4IDE2Mi44NzNMMTIzLjI5MSAxNTYuNjM2TDEyMy4xNCAxNTYuMjE5TDEyMS43NzMgMTU2LjQ0OUwxMDcuNjk5IDE3NS43NTJMODYuMzA0IDIwNC42OTlMNjkuMzY2MyAyMjIuNzc3TDY1LjI5MSAyMjQuNDMxTDU4LjI4NjcgMjIwLjc2OEw1OC45MjM1IDIxNC4yN0w2Mi44NzEzIDIwOC40OEw4Ni4zMDQgMTc4LjcwNUwxMDAuNDQgMTYwLjE1NUwxMDkuNTUxIDE0OS41MDdMMTA5LjQ2MiAxNDcuOTY3TDEwOC45NTkgMTQ3LjkyNEw0Ni42OTc3IDE4OC41MTJMMzUuNjE4MiAxODkuOTNMMzAuNzc4OCAxODUuNDRMMzEuNDE1NiAxNzguMTE1TDMzLjcwNzkgMTc1Ljc1Mkw1Mi40Mjg1IDE2Mi44NzNaIiBmaWxsPSIjMDAwMDAwIi8+Cjwvc3ZnPgo=\">\n"
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
    "    <h1 class=\"text-2xl font-semibold text-gray-900\">Claude API Gateway</h1>\n"
    "    <p class=\"mt-1 text-sm text-gray-500\">Token status &amp; rate limits</p>\n"
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

static const char HTML_SCRIPT[] =
    "<script>\n"
    "function fmtTime(ts) {\n"
    "  if (!ts) return '-';\n"
    "  var d = new Date(ts * 1000);\n"
    "  var mo = String(d.getMonth()+1).padStart(2,'0');\n"
    "  var da = String(d.getDate()).padStart(2,'0');\n"
    "  var h = String(d.getHours()).padStart(2,'0');\n"
    "  var m = String(d.getMinutes()).padStart(2,'0');\n"
    "  var s = String(d.getSeconds()).padStart(2,'0');\n"
    "  return mo+'-'+da+' '+h+':'+m+':'+s;\n"
    "}\n"
    "function fmtReset(ts) {\n"
    "  if (!ts) return '-';\n"
    "  var d = new Date(ts * 1000);\n"
    "  var mo = String(d.getMonth()+1).padStart(2,'0');\n"
    "  var da = String(d.getDate()).padStart(2,'0');\n"
    "  var h = String(d.getHours()).padStart(2,'0');\n"
    "  var m = String(d.getMinutes()).padStart(2,'0');\n"
    "  return mo+'-'+da+' '+h+':'+m;\n"
    "}\n"
    "function updateTimes() {\n"
    "  var now = Date.now() / 1000;\n"
    "  document.querySelectorAll('[data-reset]').forEach(function(el) {\n"
    "    var ts = parseInt(el.dataset.reset);\n"
    "    if (!ts) { el.textContent = '-'; return; }\n"
    "    if (now >= ts) { el.textContent = '-'; return; }\n"
    "    el.textContent = fmtReset(ts);\n"
    "  });\n"
    "  document.querySelectorAll('[data-countdown]').forEach(function(el) {\n"
    "    var ts = parseInt(el.dataset.countdown);\n"
    "    if (!ts) { el.textContent = '-'; return; }\n"
    "    var rem = Math.max(0, ts - now);\n"
    "    if (rem <= 0) { el.textContent = '-'; return; }\n"
    "    var h = Math.floor(rem / 3600);\n"
    "    var m = Math.floor((rem % 3600) / 60);\n"
    "    var s = Math.floor(rem % 60);\n"
    "    el.textContent = h > 0 ? h+'h '+m+'m '+s+'s' : m+'m '+s+'s';\n"
    "  });\n"
    "  document.querySelectorAll('[data-lastseen]').forEach(function(el) {\n"
    "    el.textContent = fmtTime(parseInt(el.dataset.lastseen));\n"
    "  });\n"
    "}\n"
    "updateTimes();\n"
    "setInterval(updateTimes, 1000);\n"
    "</script>\n";

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

        if (is_limited && t->reset_ts > 0 && now >= t->reset_ts) {
            is_limited = 0;
            newly_available = 1;
        }

        double d5 = t->util_5h >= 0 ? t->util_5h : 0;
        double d7 = t->util_7d >= 0 ? t->util_7d : 0;
        int p5 = (int)(d5 * 100); if (p5 > 100) p5 = 100;
        int p7 = (int)(d7 * 100); if (p7 > 100) p7 = 100;

        char display[32];
        display_prefix(t->prefix, display, sizeof(display));

        int stale = (now - t->last_seen > 5 * 3600);

        /* Status badge */
        const char *badge_class, *badge_text;
        if (t->banned) {
            badge_class = "bg-red-100 text-red-700 border-red-200";
            badge_text = "Banned";
        } else if (is_limited) {
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

        /* Reset timestamp: only show if currently rate limited */
        long reset_ts_val = (is_limited && t->reset_ts > 0) ? t->reset_ts : 0;
        /* Countdown: only show if currently rate limited */
        long countdown_val = (is_limited && t->reset_ts > 0) ? t->reset_ts : 0;

        const char *row_class, *row_title = "";
        if (t->banned) {
            row_class = "hover:bg-gray-50/50 transition-colors line-through opacity-50";
        } else if (stale) {
            row_class = "hover:bg-gray-50/50 transition-colors opacity-50";
            row_title = " title=\"No activity in the past 5 hours — usage data may be outdated\"";
        } else {
            row_class = "hover:bg-gray-50/50 transition-colors";
        }
        n += snprintf(buf + n, maxlen - n,
            "          <tr class=\"%s\"%s>\n", row_class, row_title);
        n += snprintf(buf + n, maxlen - n,
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

        /* Resets At, Countdown, Last Seen — raw timestamps for JS */
        n += snprintf(buf + n, maxlen - n,
            "            <td class=\"px-4 py-3 text-sm text-gray-600\" data-reset=\"%ld\">-</td>\n"
            "            <td class=\"px-4 py-3 text-sm text-gray-600\" data-countdown=\"%ld\">-</td>\n"
            "            <td class=\"px-4 py-3 text-xs text-gray-400 mono\" data-lastseen=\"%ld\">-</td>\n"
            "          </tr>\n",
            reset_ts_val, countdown_val, (long)t->last_seen);
    }

    n += snprintf(buf + n, maxlen - n, "%s", HTML_TABLE_END);

    if (has_footnote) {
        n += snprintf(buf + n, maxlen - n,
            "  <p class=\"mt-3 text-xs text-gray-400\">"
            "* Newly available &mdash; the rate limit reset time has passed but "
            "usage data may still show high utilization until the next API response updates it."
            "</p>\n");
    }

    n += snprintf(buf + n, maxlen - n, "%s", HTML_SCRIPT);
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
            "\"window\":\"%s\",\"last_seen\":%ld,"
            "\"banned\":%s}",
            display,
            is_limited ? "true" : "false",
            newly_available ? "true" : "false",
            remaining, t->reset_ts,
            t->util_5h >= 0 ? t->util_5h : 0,
            t->util_7d >= 0 ? t->util_7d : 0,
            t->status_5h, t->status_7d,
            t->window, (long)t->last_seen,
            t->banned ? "true" : "false");
    }
    n += snprintf(buf + n, maxlen - n, "]}");
    return n;
}
