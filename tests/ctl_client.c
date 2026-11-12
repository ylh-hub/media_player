#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/run/media_player.sock";
    const char *cmd  = argc > 2 ? argv[2] : "status";
    struct sockaddr_un a;
    char line[300];
    char buf[512];
    int fd;
    int len;
    ssize_t n;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);

    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    len = snprintf(line, sizeof(line), "%s\n", cmd);
    if (write(fd, line, (size_t)len) != len) {
        perror("write");
        close(fd);
        return 1;
    }

    n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }

    close(fd);
    return 0;
}
