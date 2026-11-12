#define _GNU_SOURCE

#include "ctl_unix.h"
#include "ctl_core.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define UNIX_MAX_CONN 8
#define UNIX_BUF      1024

typedef struct {
    int fd;
    int id;
    unsigned gen;
    char buf[UNIX_BUF];
    int len;
    unsigned seq;
} conn_t;

static conn_t g_conns[UNIX_MAX_CONN];
static int g_listen_fd = -1;
static char g_path[108];
static volatile int g_run = 1;
static pthread_t g_tid;
static int g_started = 0;

static void reset_conns(void)
{
    int i;
    for (i = 0; i < UNIX_MAX_CONN; i++) {
        g_conns[i].fd = -1;
        g_conns[i].id = i;
        g_conns[i].gen = 0;
        g_conns[i].len = 0;
        g_conns[i].seq = 0;
    }
}

static void conn_close(int i)
{
    conn_t *c = &g_conns[i];
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
        c->gen++;
        c->len = 0;
    }
}

static int probe_existing(const char *path)
{
    int fd;
    struct sockaddr_un addr;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return 0;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}

static int try_listen(const char *path)
{
    int fd;
    struct sockaddr_un addr;

    if (probe_existing(path))
        return -1;                 /* 已有实例，绝不抢 */

    unlink(path);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    chmod(path, 0660);

    if (listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }

    g_listen_fd = fd;
    snprintf(g_path, sizeof(g_path), "%s", path);
    return 0;
}

static int peer_allowed(int fd)
{
    struct ucred cred;
    socklen_t len = sizeof(cred);

    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
        return 0;

    /* 只允许 root 或当前用户 */
    return (cred.uid == 0 || cred.uid == getuid()) ? 1 : 0;
}

static void accept_new(void)
{
    int fd;
    int i;

    fd = accept(g_listen_fd, NULL, NULL);
    if (fd < 0)
        return;

    if (!peer_allowed(fd)) {
        close(fd);
        return;
    }

    for (i = 0; i < UNIX_MAX_CONN; i++) {
        if (g_conns[i].fd < 0) {
            g_conns[i].fd = fd;
            g_conns[i].gen++;
            g_conns[i].len = 0;
            g_conns[i].seq = 0;
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
            return;
        }
    }
    close(fd);                     /* 连接数满 */
}

static void handle_line(int i, char *line)
{
    conn_t *c = &g_conns[i];
    int rc;

    line[strcspn(line, "\r")] = '\0';
    if (line[0] == '\0')
        return;

    if (strcmp(line, "quit") == 0) {
        conn_close(i);
        return;
    }

    rc = ctl_parse_line(line, CTL_TRANSPORT_UNIX, c->id, c->seq++, NULL, 0);
    if (rc != CTL_OK) {
        ctl_done_t d;
        memset(&d, 0, sizeof(d));
        d.seq = c->seq - 1;
        d.conn_id = c->id;
        d.code = rc;
        (void)ctl_push_done(CTL_TRANSPORT_UNIX, &d);
    }
}

static void handle_conn(int i)
{
    conn_t *c = &g_conns[i];
    char tmp[256];

    for (;;) {
        ssize_t n = read(c->fd, tmp, sizeof(tmp));
        int start = 0;
        int k;

        if (n == 0) {
            conn_close(i);
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return;
            conn_close(i);
            return;
        }

        if (c->len + (int)n > UNIX_BUF) {
            c->len = 0;            /* 超长，丢弃重同步 */
            continue;
        }
        memcpy(c->buf + c->len, tmp, (size_t)n);
        c->len += (int)n;

        for (k = 0; k < c->len; k++) {
            if (c->buf[k] != '\n')
                continue;
            c->buf[k] = '\0';
            handle_line(i, c->buf + start);
            if (c->fd < 0)
                return;
            start = k + 1;
        }
        if (start > 0) {
            c->len -= start;
            memmove(c->buf, c->buf + start, (size_t)c->len);
        }
    }
}

static void drain_done(void)
{
    ctl_done_t d;

    while (ctl_take_done(CTL_TRANSPORT_UNIX, &d) == 1) {
        char line[CTL_LINE_MAX + 8];
        int i;

        if (d.conn_id < 0 || d.conn_id >= UNIX_MAX_CONN)
            continue;
        i = d.conn_id;
        if (g_conns[i].fd < 0)
            continue;              /* 连接已换/已断，丢弃 */

        if (d.payload[0] != '\0') {
            snprintf(line, sizeof(line), "%s\n", d.payload);
        } else if (d.code == CTL_OK) {
            snprintf(line, sizeof(line), "OK seq=%u\n", d.seq);
        } else {
            snprintf(line, sizeof(line), "ERR %d seq=%u\n", d.code, d.seq);
        }

        (void)write(g_conns[i].fd, line, strlen(line));
    }
}

static void *io_thread(void *arg)
{
    (void)arg;

    while (g_run) {
        struct pollfd pfds[1 + UNIX_MAX_CONN];
        int idx[1 + UNIX_MAX_CONN];
        int n = 0;
        int i;
        int r;

        pfds[n].fd = g_listen_fd;
        pfds[n].events = POLLIN;
        idx[n] = -1;
        n++;

        for (i = 0; i < UNIX_MAX_CONN; i++) {
            if (g_conns[i].fd >= 0) {
                pfds[n].fd = g_conns[i].fd;
                pfds[n].events = POLLIN;
                idx[n] = i;
                n++;
            }
        }

        r = poll(pfds, (nfds_t)n, 100);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (r == 0) {
            drain_done();
            continue;
        }

        for (i = 0; i < n; i++) {
            if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            if (idx[i] < 0)
                accept_new();
            else
                handle_conn(idx[i]);
        }
        drain_done();
    }
    return NULL;
}

int ctl_unix_start(void)
{
    reset_conns();

    if (try_listen("/run/media_player.sock") != 0 &&
        try_listen("/tmp/media_player.sock") != 0) {
        fprintf(stderr, "[ctl] unix socket unavailable\n");
        return -1;
    }

    g_run = 1;
    if (pthread_create(&g_tid, NULL, io_thread, NULL) != 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }
    g_started = 1;
    fprintf(stderr, "[ctl] unix socket: %s\n", g_path);
    return 0;
}

void ctl_unix_stop(void)
{
    if (!g_started)
        return;
    g_run = 0;
    (void)pthread_join(g_tid, NULL);
    if (g_listen_fd >= 0)
        close(g_listen_fd);
    g_listen_fd = -1;
    unlink(g_path);
    g_started = 0;
}
