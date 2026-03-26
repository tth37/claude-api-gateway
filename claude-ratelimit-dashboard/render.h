#ifndef RENDER_H
#define RENDER_H

#include <time.h>
#include <pthread.h>

#define MAX_TOKENS 64

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

extern token_state_t g_tokens[MAX_TOKENS];
extern pthread_mutex_t g_lock;

void fmt_shanghai(time_t ts, char *buf, int maxlen, const char *fmt);
int  render_html(char *buf, int maxlen);
int  render_json(char *buf, int maxlen);

#endif
