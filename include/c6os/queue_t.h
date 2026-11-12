#ifndef C6OS_QUEUE_T_H
#define C6OS_QUEUE_T_H

#include "c6os/queue.h"

/* c6 泛型值队列：调用方提供 T buf[cap]，每个元素按 sizeof(T) 深拷贝。 */
struct Queue@T {
    OsQueue q;
    T *buf;
    u32 cap;
};

|T| int q_init(Queue@T *self, T *buf, u32 cap) {
    self->buf = buf;
    self->cap = cap;
    return os_queue_init_ex(&self->q, (void *)buf, cap, (u32)sizeof(T));
}

|T| int q_put(Queue@T *self, T v, u32 timeout) {
    return os_queue_put_item(&self->q, (const void *)&v, timeout);
}

|T| int q_get(Queue@T *self, T *out, u32 timeout) {
    return os_queue_get_item(&self->q, (void *)out, timeout);
}

#endif
