#ifndef C6OS_CONFIG_H
#define C6OS_CONFIG_H

/* 可覆盖的编译期配置；目标侧用 -D 覆盖。 */
#ifndef C6OS_MAX_CORES
#define C6OS_MAX_CORES 2
#endif
#ifndef C6OS_MAX_TASKS
#define C6OS_MAX_TASKS 16
#endif
#ifndef C6OS_PRIOS
#define C6OS_PRIOS 32
#endif
#ifndef C6OS_TICK_HZ
#define C6OS_TICK_HZ 100
#endif
#ifndef C6OS_TIMESLICE
#define C6OS_TIMESLICE 10
#endif
#ifndef C6OS_STACK_DEFAULT
#define C6OS_STACK_DEFAULT 8192
#endif
#ifndef C6OS_HEAP_SIZE
#define C6OS_HEAP_SIZE 65536
#endif

#endif
