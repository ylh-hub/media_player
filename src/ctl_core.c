#include "ctl_core.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 队列全部由 g_lock 保护；GUI/网络/IO 线程通过下面的接口访问 */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static ctl_cmd_t g_cmd[CTL_CMD_QUEUE];
static int g_cmd_head, g_cmd_tail, g_cmd_count;

static ctl_done_t g_done[CTL_TRANSPORT_MAX][CTL_DONE_QUEUE];
static int g_done_head[CTL_TRANSPORT_MAX];
static int g_done_tail[CTL_TRANSPORT_MAX];
static int g_done_count[CTL_TRANSPORT_MAX];

static char g_status[CTL_LINE_MAX] = "{\"state\":\"stopped\"}";

int ctl_push_cmd(const ctl_cmd_t *c)
{
    int rc = 0;

    if (c == NULL)
        return -1;

    pthread_mutex_lock(&g_lock);
    if (g_cmd_count >= CTL_CMD_QUEUE) {
        rc = -1;
    } else {
        g_cmd[g_cmd_tail] = *c;
        g_cmd_tail = (g_cmd_tail + 1) % CTL_CMD_QUEUE;
        g_cmd_count++;
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

int ctl_take_cmd(ctl_cmd_t *out, int max)
{
    int n = 0;

    if (out == NULL || max <= 0)
        return 0;

    pthread_mutex_lock(&g_lock);
    while (n < max && g_cmd_count > 0) {
        out[n++] = g_cmd[g_cmd_head];
        g_cmd_head = (g_cmd_head + 1) % CTL_CMD_QUEUE;
        g_cmd_count--;
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}

int ctl_push_done(int transport, const ctl_done_t *d)
{
    int rc = 0;

    if (d == NULL || transport < 0 || transport >= CTL_TRANSPORT_MAX)
        return -1;

    pthread_mutex_lock(&g_lock);
    if (g_done_count[transport] >= CTL_DONE_QUEUE) {
        rc = -1;
    } else {
        g_done[transport][g_done_tail[transport]] = *d;
        g_done_tail[transport] = (g_done_tail[transport] + 1) % CTL_DONE_QUEUE;
        g_done_count[transport]++;
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

int ctl_take_done(int transport, ctl_done_t *out)
{
    int got = 0;

    if (out == NULL || transport < 0 || transport >= CTL_TRANSPORT_MAX)
        return 0;

    pthread_mutex_lock(&g_lock);
    if (g_done_count[transport] > 0) {
        *out = g_done[transport][g_done_head[transport]];
        g_done_head[transport] = (g_done_head[transport] + 1) % CTL_DONE_QUEUE;
        g_done_count[transport]--;
        got = 1;
    }
    pthread_mutex_unlock(&g_lock);
    return got;
}

void ctl_set_status(const char *text)
{
    if (text == NULL)
        return;
    pthread_mutex_lock(&g_lock);
    snprintf(g_status, sizeof(g_status), "%s", text);
    pthread_mutex_unlock(&g_lock);
}

int ctl_status_text(char *buf, int cap)
{
    if (buf == NULL || cap <= 0)
        return -1;
    pthread_mutex_lock(&g_lock);
    snprintf(buf, (size_t)cap, "%s", g_status);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int parse_int(const char *s, int *out)
{
    char *end = NULL;
    long v;

    if (s == NULL || *s == '\0')
        return -1;

    v = strtol(s, &end, 10);
    if (end == s || *end != '\0')
        return -1;
    if (v < -2147483647L || v > 2147483647L)
        return -1;

    *out = (int)v;
    return 0;
}

int ctl_parse_line(const char *line, int transport, int conn_id, unsigned seq,
                   char *err, int cap)
{
    char buf[CTL_LINE_MAX];
    char *save = NULL;
    char *tok;
    char *arg;
    ctl_cmd_t c;
    int v;

    (void)err;
    (void)cap;

    if (line == NULL)
        return CTL_E_SYNTAX;

    while (*line == ' ' || *line == '\t')
        line++;
    if (*line == '\0')
        return CTL_E_SYNTAX;

    snprintf(buf, sizeof(buf), "%s", line);

    tok = strtok_r(buf, " \t\r\n", &save);
    if (tok == NULL)
        return CTL_E_SYNTAX;
    arg = strtok_r(NULL, " \t\r\n", &save);

    memset(&c, 0, sizeof(c));
    c.conn_id = conn_id;
    c.seq = seq;
    c.transport = transport;

    if (strcmp(tok, "play") == 0) {
        if (arg == NULL) {
            c.id = CTL_PLAY;
        } else {
            if (parse_int(arg, &v) != 0)
                return CTL_E_ARG;
            if (v < 0)
                return CTL_E_RANGE;
            c.id = CTL_PLAY_INDEX;
            c.arg = v;
        }
    } else if (strcmp(tok, "pause") == 0) {
        c.id = CTL_PAUSE;
    } else if (strcmp(tok, "toggle") == 0) {
        c.id = CTL_TOGGLE;
    } else if (strcmp(tok, "stop") == 0) {
        c.id = CTL_STOP;
    } else if (strcmp(tok, "next") == 0) {
        c.id = CTL_NEXT;
    } else if (strcmp(tok, "prev") == 0) {
        c.id = CTL_PREV;
    } else if (strcmp(tok, "vol") == 0) {
        if (arg == NULL || parse_int(arg, &v) != 0)
            return CTL_E_ARG;
        if (v < 0 || v > 100)
            return CTL_E_RANGE;
        c.id = CTL_VOL;
        c.arg = v;
    } else if (strcmp(tok, "seek") == 0) {
        size_t n;
        int pct = 0;

        if (arg == NULL)
            return CTL_E_ARG;
        n = strlen(arg);
        if (n > 0 && arg[n - 1] == '%') {
            pct = 1;
            arg[n - 1] = '\0';
        }
        if (parse_int(arg, &v) != 0)
            return CTL_E_ARG;
        if (v <= 0)
            return CTL_E_RANGE;
        c.id = CTL_SEEK;
        c.arg = pct ? -v : v;   /* 负数表示百分比 */
    } else if (strcmp(tok, "status") == 0) {
        c.id = CTL_STATUS;
    } else if (strcmp(tok, "list") == 0) {
        c.id = CTL_LIST;
    } else {
        return CTL_E_UNKNOWN;
    }

    if (ctl_push_cmd(&c) != 0)
        return CTL_E_BUSY;

    return CTL_OK;
}
