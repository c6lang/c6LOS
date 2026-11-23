/* c6LOS 的 lwIP sys_arch 接口（NO_SYS=0 时用）
 *
 * 目的（docs/wifi-plan.md §207）：让 lwIP 跑成"有自己的 tcpip 线程 + BSD socket"
 * 的模式，这样 /home/winger/c6/std/net/tcp.c6、https.c6 那种 POSIX 薄封装
 * 就能直接用（最终目标是 https://qq.com）。
 *
 * 原语全落在我们自己的内核上（call0），本文件属于 windowed 侧，所以实现里
 * 一律通过 port/s3/w2c.S 的 s3_w2c_* 壳去调 os_sem/os_mutex/os_queue/os_task。
 *
 * 句柄语义照 lwIP contrib 的 unix 端口：typedef 成"指针的指针"，
 * 于是 sys_*_valid(&h) / sys_*_set_invalid(&h) 这种用法能正常工作。
 */
#ifndef C6_LWIP_SYS_ARCH_H
#define C6_LWIP_SYS_ARCH_H

#include "lwip/opt.h"
#include "lwip/err.h"

struct c6_lwip_sem;
struct c6_lwip_mutex;
struct c6_lwip_mbox;

typedef struct c6_lwip_sem  *sys_sem_t;
typedef struct c6_lwip_mutex *sys_mutex_t;
typedef struct c6_lwip_mbox *sys_mbox_t;
typedef int sys_thread_t;

#define SYS_MBOX_NULL   ((sys_mbox_t)0)
#define SYS_SEM_NULL    ((sys_sem_t)0)

#define sys_sem_valid(sem)       (((sem) != 0) && (*(sem) != 0))
#define sys_sem_set_invalid(sem) do { if ((sem) != 0) { *(sem) = 0; } } while (0)
#define sys_mutex_valid(mtx)     (((mtx) != 0) && (*(mtx) != 0))
#define sys_mutex_set_invalid(mtx) do { if ((mtx) != 0) { *(mtx) = 0; } } while (0)
#define sys_mbox_valid(mbox)     (((mbox) != 0) && (*(mbox) != 0))
#define sys_mbox_set_invalid(mbox) do { if ((mbox) != 0) { *(mbox) = 0; } } while (0)

/* 保护临界区：我们的 lwipopts.h 里 SYS_LIGHTWEIGHT_PROT=0，lwIP 不会调它；
 * 真要用的时候再接内核的 port_irq_save()（call0，需要再加一个 w2c 壳）。 */
/* 保护临界区：我们的 lwipopts.h 里 SYS_LIGHTWEIGHT_PROT=0，lwIP 不会调它；
 * 真要用的时候再接内核的 port_irq_save()（call0，需要再加一个 w2c 壳）。 */
typedef int sys_prot_t;

/* 注意：sys_sem_new / sys_mutex_new / sys_mbox_new / sys_thread_new 这些**函数原型
 * 全部由 lwIP 自己的 lwip/sys.h 声明**，本文件只负责类型和 valid/invalid 宏。
 * （lwip/sys.h 会先 include 本文件、之后才 typedef lwip_thread_fn，
 *   所以在本文件里写 sys_thread_new 的原型会报"unknown type name lwip_thread_fn"。） */

#endif /* C6_LWIP_SYS_ARCH_H */
