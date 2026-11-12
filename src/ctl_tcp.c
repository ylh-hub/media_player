#define _GNU_SOURCE

#include "ctl_tcp.h"
#include "ctl_core.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define TCP_PORT     19000
#define TCP_MAX_CONN 8
#define TCP_IN_BUF   1024
#define TCP_OUT_BUF  1024
#define TCP_MAX_EVT  16

typedef struct {
    int fd;
    int slot;
    char in[TCP_IN_BUF];
    int in_len;
    char out[TCP_OUT_BUF];
    int out_len;
    unsigned seq;
} tconn_t;

static tconn_t g_conns[TCP_MAX_CONN];
static int g_listen = -1;
static int g_epfd = -1;
static volatile int g_run = 1;
static pthread_t g_tid;
static int g_started = 0;

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0)
        (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void conn_reset(tconn_t *c, int slot)
{
    c->fd = -1;
    c->slot = slot;
    c->in_len = 0;
    c->out_len = 0;
    c->seq = 0;
}

static void conn_close(tconn_t *c)
{
    if (c->fd < 0)
        return;
    (void)epoll_ctl(g_epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    conn_reset(c, c->slot);
}

static void flush_out(tconn_t *c)
{
    int off = 0;

    while (off < c->out_len) {
        ssize_t n = write(c->fd, c->out + off, (size_t)(c->out_len - off));
        if (n > 0) {
            off += (int)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            break;
        conn_close(c);
        return;
    }

    if (off > 0) {
        c->out_len -= off;
        if (c->out_len > 0)
            memmove(c->out, c->out + off, (size_t)c->out_len);
    }

    if (c->out_len == 0) {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.fd = c->fd;
        (void)epoll_ctl(g_epfd, EPOLL_CTL_MOD, c->fd, &ev);
    }
}

static void append_out(tconn_t *c, const char *s)
{
    int len = (int)strlen(s);

    if (c->out_len + len > TCP_OUT_BUF)
        return;                   /* 输出满，丢弃本响应 */

    memcpy(c->out + c->out_len, s, (size_t)len);
    c->out_len += len;
    flush_out(c);
}

static void accept_new(void)
{
    int fd;
    int i;
    struct epoll_event ev;

    fd = accept(g_listen, NULL, NULL);
    if (fd < 0)
        return;

    for (i = 0; i < TCP_MAX_CONN; i++) {
        if (g_conns[i].fd < 0)
            break;
    }
    if (i >= TCP_MAX_CONN) {
        close(fd);
        return;
    }

    set_nonblock(fd);
    conn_reset(&g_conns[i], i);
    g_conns[i].fd = fd;

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    (void)epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev);
}

static void handle_line(tconn_t *c, char *line)
{
    int rc;

    line[strcspn(line, "\r")] = '\0';
    if (line[0] == '\0')
        return;

    if (strcmp(line, "quit") == 0) {
        conn_close(c);
        return;
    }

    rc = ctl_parse_line(line, CTL_TRANSPORT_TCP, c->slot, c->seq++, NULL, 0);
    if (rc != CTL_OK) {
        ctl_done_t d;
        memset(&d, 0, sizeof(d));
        d.seq = c->seq - 1;
        d.conn_id = c->slot;
        d.code = rc;
        (void)ctl_push_done(CTL_TRANSPORT_TCP, &d);
    }
}

static void handle_read(tconn_t *c)
{
    char tmp[256];

    for (;;) {
        ssize_t n = read(c->fd, tmp, sizeof(tmp));
        int start = 0;
        int k;

        if (n == 0) {
            conn_close(c);
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return;
            conn_close(c);
            return;
        }

        if (c->in_len + (int)n > TCP_IN_BUF) {
            c->in_len = 0;
            continue;
        }
        memcpy(c->in + c->in_len, tmp, (size_t)n);
        c->in_len += (int)n;

        for (k = 0; k < c->in_len; k++) {
            if (c->in[k] != '\n')
                continue;
            c->in[k] = '\0';
            handle_line(c, c->in + start);
            if (c->fd < 0)
                return;
            start = k + 1;
        }
        if (start > 0) {
            c->in_len -= start;
            memmove(c->in, c->in + start, (size_t)c->in_len);
        }
    }
}

static void drain_done(void)
{
    ctl_done_t d;

    while (ctl_take_done(CTL_TRANSPORT_TCP, &d) == 1) {
        char line[CTL_LINE_MAX + 8];
        int slot;

        if (d.conn_id < 0 || d.conn_id >= TCP_MAX_CONN)
            continue;
        slot = d.conn_id;
        if (g_conns[slot].fd < 0)
            continue;

        if (d.payload[0] != '\0')
            snprintf(line, sizeof(line), "%s\n", d.payload);
        else if (d.code == CTL_OK)
            snprintf(line, sizeof(line), "OK seq=%u\n", d.seq);
        else
            snprintf(line, sizeof(line), "ERR %d seq=%u\n", d.code, d.seq);

        append_out(&g_conns[slot], line);
    }
}

static void *io_thread(void *arg)
{
    struct epoll_event evs[TCP_MAX_EVT];
    int i;

    (void)arg;
    signal(SIGPIPE, SIG_IGN);

    while (g_run) {
        int n = epoll_wait(g_epfd, evs, TCP_MAX_EVT, 100);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        for (i = 0; i < n; i++) {
            int fd = evs[i].data.fd;
            uint32_t ev = evs[i].events;

            if (fd == g_listen) {
                accept_new();
                continue;
            }

            {
                int slot;
                for (slot = 0; slot < TCP_MAX_CONN; slot++) {
                    if (g_conns[slot].fd == fd)
                        break;
                }
                if (slot >= TCP_MAX_CONN)
                    continue;

                if (ev & (EPOLLHUP | EPOLLERR)) {
                    conn_close(&g_conns[slot]);
                } else {
                    if (ev & EPOLLOUT)
                        flush_out(&g_conns[slot]);
                    if (ev & EPOLLIN)
                        handle_read(&g_conns[slot]);
                }
            }
        }
        drain_done();
    }
    return NULL;
}

int ctl_tcp_start(void)
{
    struct sockaddr_in addr;
    struct epoll_event ev;
    int opt = 1;
    int i;

    for (i = 0; i < TCP_MAX_CONN; i++)
        conn_reset(&g_conns[i], i);

    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen < 0) {
        perror("[ctl] tcp socket");
        return -1;
    }

    (void)setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(TCP_PORT);

    if (bind(g_listen, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("[ctl] tcp bind");
        close(g_listen);
        g_listen = -1;
        return -1;
    }

    if (listen(g_listen, 8) != 0) {
        perror("[ctl] tcp listen");
        close(g_listen);
        g_listen = -1;
        return -1;
    }

    set_nonblock(g_listen);

    g_epfd = epoll_create1(0);
    if (g_epfd < 0) {
        perror("[ctl] epoll_create1");
        close(g_listen);
        g_listen = -1;
        return -1;
    }

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = g_listen;
    if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, g_listen, &ev) != 0) {
        close(g_epfd);
        close(g_listen);
        g_epfd = -1;
        g_listen = -1;
        return -1;
    }

    g_run = 1;
    if (pthread_create(&g_tid, NULL, io_thread, NULL) != 0) {
        close(g_epfd);
        close(g_listen);
        g_epfd = -1;
        g_listen = -1;
        return -1;
    }
    g_started = 1;
    fprintf(stderr, "[ctl] tcp: 0.0.0.0:%d\n", TCP_PORT);
    return 0;
}

void ctl_tcp_stop(void)
{
    if (!g_started)
        return;
    g_run = 0;
    (void)pthread_join(g_tid, NULL);
    if (g_listen >= 0)
        close(g_listen);
    if (g_epfd >= 0)
        close(g_epfd);
    g_listen = -1;
    g_epfd = -1;
    g_started = 0;
}
