/* M42 验收：AF_UNIX SCM_RIGHTS fd 传递。
 * 父建 AF_UNIX DGRAM socket 对；子创建一个管道，把写端通过 sendmsg 的
 * SCM_RIGHTS cmsg 传给父；父 recvmsg 收到 fd，往里写，子从管道读端读回，
 * 证明收到的 fd 是真正可用的内核对象引用。 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static int total;

#define CHECK(c) do { total++; if (!(c)) { \
    failures++; dprintf(1, "FAIL: %s (line %d, errno=%d)\n", #c, __LINE__, errno); } \
} while (0)

static void addr(struct sockaddr_un *a, const char *p)
{
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    strncpy(a->sun_path, p, sizeof(a->sun_path) - 1);
}

int main(void)
{
    struct sockaddr_un sa, sb;
    struct msghdr mh;
    struct iovec iov;
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cm;
    int a, b, pipefd[2];
    char byte = 'X';
    char payload = 'P';
    pid_t c;
    int st;

    a = socket(AF_UNIX, SOCK_DGRAM, 0);
    b = socket(AF_UNIX, SOCK_DGRAM, 0);
    CHECK(a >= 0 && b >= 0);
    addr(&sa, "/tmp/m42-a");
    addr(&sb, "/tmp/m42-b");
    CHECK(bind(a, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    CHECK(bind(b, (struct sockaddr *)&sb, sizeof(sb)) == 0);
    CHECK(connect(a, (struct sockaddr *)&sb, sizeof(sb)) == 0);
    CHECK(connect(b, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    CHECK(pipe(pipefd) == 0);

    c = fork();
    CHECK(c >= 0);
    if (c == 0) {
        /* 子：把管道写端传给父，然后从读端等数据 */
        struct msghdr m;
        struct iovec v;
        char cb[CMSG_SPACE(sizeof(int))];
        char p = 'P';
        struct cmsghdr *cmsg;

        v.iov_base = &p; v.iov_len = 1;
        memset(&m, 0, sizeof(m));
        m.msg_iov = &v; m.msg_iovlen = 1;
        m.msg_control = cb; m.msg_controllen = sizeof(cb);
        cmsg = CMSG_FIRSTHDR(&m);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        *(int *)CMSG_DATA(cmsg) = pipefd[1];
        m.msg_controllen = cmsg->cmsg_len;
        if (sendmsg(a, &m, 0) != 1) { _exit(2); }
        if (read(pipefd[0], &byte, 1) != 1) { _exit(3); }
        _exit(byte == 'X' ? 0 : 4);
    }

    /* 父：收 fd，写入，等子确认 */
    close(pipefd[0]); close(pipefd[1]);
    iov.iov_base = &payload; iov.iov_len = 1;
    memset(&mh, 0, sizeof(mh));
    memset(cbuf, 0, sizeof(cbuf));
    mh.msg_iov = &iov; mh.msg_iovlen = 1;
    mh.msg_control = cbuf; mh.msg_controllen = sizeof(cbuf);
    CHECK(recvmsg(b, &mh, 0) == 1);
    CHECK(payload == 'P');
    cm = CMSG_FIRSTHDR(&mh);
    CHECK(cm != 0);
    if (cm != 0) {
        int passed_fd = -1;

        CHECK(cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS);
        memcpy(&passed_fd, CMSG_DATA(cm), sizeof(int));
        CHECK(passed_fd >= 0);
        if (passed_fd >= 0) {
            CHECK(write(passed_fd, "X", 1) == 1);
            close(passed_fd);
        }
    }
    CHECK(waitpid(c, &st, 0) == c);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(a); close(b);

    if (failures == 0) {
        dprintf(1, "PASS: m42scmright (%d/%d checks)\n", total, total);
    } else {
        dprintf(1, "FAIL: m42scmright (%d/%d)\n", total - failures, total);
    }
    return failures == 0 ? 0 : 1;
}
