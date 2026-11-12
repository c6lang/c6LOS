/* 最小串口 shell：只做行解析与内置命令，输出走 port_puts。 */
#include "c6os/shell.h"
#include "c6os/task.h"
#include "c6os/tick.h"
#include "c6os/port.h"
#include "c6os/log.h"
#include "c6os/cobs.h"

static OsHeap *g_heap;

void os_shell_bind_heap(OsHeap *h)
{
    g_heap = h;
}

static void out_u32(u32 v)
{
    char b[12];
    int i = 0;

    if (v == 0u) {
        port_putc('0');
        return;
    }
    while (v > 0u && i < 12) {
        b[i] = (char)('0' + (v % 10u));
        i++;
        v = v / 10u;
    }
    while (i > 0) {
        i--;
        port_putc((int)b[i]);
    }
}

static int same(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static void cmd_help(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    port_puts("help tick ps free log date reboot\n");
}

static void cmd_tick(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    port_puts("tick=");
    out_u32((u32)os_tick_get());
    port_puts("\n");
}

static char state_ch(u8 s)
{
    if (s == OS_TASK_READY) {
        return 'R';
    }
    if (s == OS_TASK_RUNNING) {
        return 'X';
    }
    if (s == OS_TASK_BLOCKED) {
        return 'B';
    }
    if (s == OS_TASK_SUSP) {
        return 'S';
    }
    if (s == OS_TASK_DEAD) {
        return 'D';
    }
    return '?';
}

static void cmd_ps(int argc, char **argv)
{
    u32 id = 1;

    (void)argc;
    (void)argv;
    while (id < 64u) {
        OsTask *t = os_task_get(id);

        if (t != 0 && t->state != (u8)OS_TASK_FREE) {
            port_putc(' ');
            out_u32(t->id);
            port_putc(' ');
            out_u32((u32)t->prio);
            port_putc(' ');
            port_putc((int)state_ch(t->state));
            port_putc(' ');
            port_puts(t->name);
            port_puts("\n");
        }
        id++;
    }
}

static void cmd_free(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (g_heap == 0) {
        port_puts("heap: none\n");
        return;
    }
    port_puts("used=");
    out_u32(os_heap_used(g_heap));
    port_puts(" peak=");
    out_u32(os_heap_peak(g_heap));
    port_puts(" maxfree=");
    out_u32(os_heap_max_free(g_heap));
    port_puts("\n");
}

static void cmd_date(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    port_puts("uptime_ms=");
    out_u32((u32)os_ticks_to_ms(os_tick_get()));
    port_puts("\n");
}

static void cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    port_puts("reboot\n");
    port_reboot();
}

static void cmd_log(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    klog_dump();
}

typedef struct {
    const char *name;
    void (*fn)(int argc, char **argv);
} ShellCmd;

static const ShellCmd g_cmds[] = {
    {"help", cmd_help},
    {"tick", cmd_tick},
    {"ps", cmd_ps},
    {"free", cmd_free},
    {"log", cmd_log},
    {"date", cmd_date},
    {"reboot", cmd_reboot},
};

static int split(char *s, char *argv[], int max)
{
    int n = 0;

    while (*s != '\0' && n < max) {
        while (*s == ' ' || *s == '\t') {
            *s = '\0';
            s++;
        }
        if (*s == '\0') {
            break;
        }
        argv[n] = s;
        n++;
        while (*s != '\0' && *s != ' ' && *s != '\t') {
            s++;
        }
    }
    return n;
}

int os_shell_exec(const char *line)
{
    char buf[128];
    char *argv[8];
    int argc = 0;
    int i = 0;
    u32 k = 0;

    while (line[i] != '\0' && i < 127) {
        buf[i] = line[i];
        i++;
    }
    buf[i] = '\0';
    if (i > 0 && buf[i - 1] == '\r') {
        buf[i - 1] = '\0';
    }
    argc = split(buf, argv, 8);
    if (argc == 0) {
        return 0;
    }
    while (k < (u32)(sizeof(g_cmds) / sizeof(g_cmds[0]))) {
        if (same(argv[0], g_cmds[k].name) != 0) {
            g_cmds[k].fn(argc, argv);
            return 0;
        }
        k++;
    }
    port_puts("unknown: ");
    port_puts(argv[0]);
    port_puts("\n");
    return -1;
}

int os_shell_exec_frame(const u8 *frame, u32 n)
{
    u8 line[128];
    int dn = 0;

    if (frame == 0 || n == 0u) {
        return -1;
    }
    dn = cobs_decode(frame, n, line, (u32)sizeof(line) - 1u);
    if (dn < 0) {
        port_puts("bad frame\n");
        return -1;
    }
    line[dn] = 0;
    return os_shell_exec((const char *)line);
}
