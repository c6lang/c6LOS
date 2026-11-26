/* G4 客户机驱动：clone 服务线程 + wl_demo 客户端（不使用 pthread/TLS）。 */
#define _GNU_SOURCE
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int c6wl_main(int argc, char **argv);
int wl_main(int argc, char **argv);

static char g_sock[256];
static int g_srv_rc;

static int srv_fn(void *arg)
{
    char *av[6];
    (void)arg;
    av[0] = (char *)"c6wl";
    av[1] = g_sock;
    av[2] = (char *)"--fb=/dev/fb0";
    av[3] = (char *)"--width=1280";
    av[4] = (char *)"--height=800";
    av[5] = NULL;
    g_srv_rc = c6wl_main(5, av);
    return 0;
}

int main(int argc, char **argv)
{
    const char *sock = "/tmp/wayland-0";
    char sc[256];
    void *stk;
    int i;
    int rc;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--socket=", 9) == 0) sock = argv[i] + 9;
    }
    snprintf(g_sock, sizeof(g_sock), "--socket=%s", sock);
    unlink(sock);
    stk = mmap(NULL, 1u << 20, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stk == MAP_FAILED) {
        dprintf(2, "c6g4demo: stack mmap failed\n");
        return 2;
    }
    if (clone(srv_fn, (char *)stk + (1u << 20),
              CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | SIGCHLD,
              NULL) < 0) {
        dprintf(2, "c6g4demo: clone failed\n");
        return 2;
    }
    for (i = 0; i < 1000; i++) {
        sched_yield();
    }
    snprintf(sc, sizeof(sc), "--socket=%s", sock);
    {
        char *av[3];
        av[0] = (char *)"wl_demo";
        av[1] = sc;
        av[2] = NULL;
        rc = wl_main(2, av);
    }
    dprintf(1, "c6g4demo: client_rc=%d server_rc=%d\n", rc, g_srv_rc);
    return rc;
}
