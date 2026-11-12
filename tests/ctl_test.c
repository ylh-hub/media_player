#include <stdio.h>
#include <string.h>
#include "ctl_core.h"

static int fails = 0;

static void ok(const char *name, int cond)
{
    printf("  %-34s %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond)
        fails++;
}

int main(void)
{
    ctl_cmd_t c, out[CTL_CMD_QUEUE];
    ctl_done_t d, got;
    char st[64];
    int n, i, rc;

    printf("control core 自测：\n");

    rc = ctl_parse_line("vol 60", CTL_TRANSPORT_UNIX, 0, 1, NULL, 0);
    n = ctl_take_cmd(out, 1);
    ok("parse vol 60", rc == CTL_OK && n == 1 && out[0].id == CTL_VOL && out[0].arg == 60);

    rc = ctl_parse_line("seek 50%", CTL_TRANSPORT_UNIX, 0, 2, NULL, 0);
    n = ctl_take_cmd(out, 1);
    ok("parse seek 50%", rc == CTL_OK && n == 1 && out[0].id == CTL_SEEK && out[0].arg == -50);

    rc = ctl_parse_line("play 3", CTL_TRANSPORT_UNIX, 0, 3, NULL, 0);
    n = ctl_take_cmd(out, 1);
    ok("parse play 3", rc == CTL_OK && n == 1 && out[0].id == CTL_PLAY_INDEX && out[0].arg == 3);

    rc = ctl_parse_line("bogus", CTL_TRANSPORT_UNIX, 0, 4, NULL, 0);
    ok("parse unknown", rc == CTL_E_UNKNOWN);

    rc = ctl_parse_line("vol 101", CTL_TRANSPORT_UNIX, 0, 5, NULL, 0);
    ok("parse vol range", rc == CTL_E_RANGE);

    rc = ctl_parse_line("seek abc", CTL_TRANSPORT_UNIX, 0, 6, NULL, 0);
    ok("parse seek bad", rc == CTL_E_ARG);

    rc = ctl_parse_line("status", CTL_TRANSPORT_UNIX, 0, 7, NULL, 0);
    n = ctl_take_cmd(out, 1);
    ok("parse status", rc == CTL_OK && n == 1 && out[0].id == CTL_STATUS);

    memset(&c, 0, sizeof(c));
    c.id = CTL_STATUS;
    for (i = 0; i < CTL_CMD_QUEUE; i++)
        (void)ctl_push_cmd(&c);
    ok("queue full", ctl_push_cmd(&c) == -1);
    n = ctl_take_cmd(out, CTL_CMD_QUEUE);
    ok("drain all", n == CTL_CMD_QUEUE);

    memset(&d, 0, sizeof(d));
    d.seq = 7;
    d.conn_id = 2;
    d.code = CTL_OK;
    snprintf(d.payload, sizeof(d.payload), "OK seq=7");
    ok("done push", ctl_push_done(CTL_TRANSPORT_UNIX, &d) == 0);
    ok("done take", ctl_take_done(CTL_TRANSPORT_UNIX, &got) == 1 && got.seq == 7 && got.conn_id == 2);
    ok("done empty", ctl_take_done(CTL_TRANSPORT_UNIX, &got) == 0);

    /* 多 transport 隔离：UNIX 的完成事件不能被 TCP 取走 */
    memset(&d, 0, sizeof(d));
    d.seq = 11;
    d.conn_id = 0;
    d.code = CTL_OK;
    snprintf(d.payload, sizeof(d.payload), "U");
    (void)ctl_push_done(CTL_TRANSPORT_UNIX, &d);

    d.seq = 22;
    snprintf(d.payload, sizeof(d.payload), "T");
    (void)ctl_push_done(CTL_TRANSPORT_TCP, &d);

    ok("unix done isolated", ctl_take_done(CTL_TRANSPORT_UNIX, &got) == 1 && got.seq == 11);
    ok("tcp done isolated", ctl_take_done(CTL_TRANSPORT_TCP, &got) == 1 && got.seq == 22);
    ok("both empty", ctl_take_done(CTL_TRANSPORT_UNIX, &got) == 0 &&
                     ctl_take_done(CTL_TRANSPORT_TCP, &got) == 0);

    ctl_set_status("{\"state\":\"playing\"}");
    ok("status get", ctl_status_text(st, sizeof(st)) == 0 && strstr(st, "playing") != NULL);

    if (fails)
        printf("FAILED (%d)\n", fails);
    else
        printf("ALL PASS\n");
    return fails ? 1 : 0;
}
