/* ESP32-S3 单核热路径 C 实现：语义与 kernel/{sem,queue,heap}.c6 一致，
 * 用于替换 c6->C99 生成代码的通用序言开销。 */
#include "c6os/sem.h"
#include "c6os/queue.h"
#include "c6os/heap.h"
#include "c6os/task.h"

#define SEM_MAGIC 0xC6E50001u
#define QUEUE_MAGIC 0x51E5C6A5u
#define Q_WAIT_GET 6u
#define Q_WAIT_PUT 7u

int os_sem_init(OsSem *s, u32 initial, u32 max) {
    if (s == 0 || initial > max) return -1;
    s->count = initial;
    s->max = max;
    s->magic = SEM_MAGIC;
    s->waiters = 0;
    return 0;
}

int os_sem_destroy(OsSem *s) {
    u32 st;
    if (s == 0 || s->magic != SEM_MAGIC) return -1;
    st = os_obj_lock();
    os_task_wake_all(s, -3);
    s->count = 0; s->max = 0; s->magic = 0; s->waiters = 0;
    os_obj_unlock(st);
    return 0;
}

int os_sem_pend(OsSem *s, u32 timeout) {
    u32 st;
    int r;
    if (s == 0 || s->magic != SEM_MAGIC) return -1;
    st = os_obj_lock();
    if (s->magic != SEM_MAGIC) { os_obj_unlock(st); return -1; }
    if (s->count > 0u) {
        s->count--;
        os_obj_unlock(st);
        return 0;
    }
    s->waiters++;
    r = os_task_block_prepare(s, timeout, 1u, 0u);
    os_obj_unlock(st);
    if (r != 0) { s->waiters--; return r; }
    r = os_task_block_wait();
    s->waiters--;
    return r;
}

int os_sem_post(OsSem *s) {
    u32 st;
    if (s == 0 || s->magic != SEM_MAGIC) return -1;
    st = os_obj_lock();
    if (s->magic != SEM_MAGIC) { os_obj_unlock(st); return -1; }
    if (s->waiters == 0u) {
        if (s->count < s->max) s->count++;
    } else if (os_task_wake_first(s, 1u, 0) == 0) {
        if (s->count < s->max) s->count++;
    }
    os_obj_unlock(st);
    return 0;
}

u32 os_sem_count(OsSem *s) { return s == 0 ? 0u : s->count; }

int os_queue_init_ex(OsQueue *q, void *storage, u32 cap, u32 item_size) {
    if (q == 0 || storage == 0 || cap == 0u || item_size == 0u ||
        item_size > 4096u || cap > 0xFFFFFFFFu / item_size) return -1;
    q->buf = (u8 *)storage;
    q->item_size = item_size;
    q->cap = cap;
    q->head = 0; q->tail = 0; q->count = 0;
    q->magic = QUEUE_MAGIC;
    q->waiters_get = 0; q->waiters_put = 0;
    return 0;
}

int os_queue_init(OsQueue *q, void **storage, u32 cap) {
    return os_queue_init_ex(q, (void *)storage, cap, (u32)sizeof(void *));
}

static void q_write(OsQueue *q, u32 idx, const void *src) {
    u8 *d = q->buf + idx * q->item_size;
    const u8 *s = (const u8 *)src;
    if (q->item_size == 4u) { *(u32 *)d = *(const u32 *)s; return; }
    {
        u32 i = 0;
        while (i < q->item_size) { d[i] = s[i]; i++; }
    }
}

static void q_read(OsQueue *q, u32 idx, void *dst) {
    const u8 *s = q->buf + idx * q->item_size;
    u8 *d = (u8 *)dst;
    if (q->item_size == 4u) { *(u32 *)d = *(const u32 *)s; return; }
    {
        u32 i = 0;
        while (i < q->item_size) { d[i] = s[i]; i++; }
    }
}

int os_queue_destroy(OsQueue *q) {
    u32 st;
    if (q == 0 || q->magic != QUEUE_MAGIC) return -1;
    st = os_obj_lock();
    os_task_wake_all(q, -3);
    q->buf = 0; q->item_size = 0; q->cap = 0;
    q->head = 0; q->tail = 0; q->count = 0;
    q->magic = 0; q->waiters_get = 0; q->waiters_put = 0;
    os_obj_unlock(st);
    return 0;
}

int os_queue_put_item(OsQueue *q, const void *item, u32 timeout) {
    if (q == 0 || q->magic != QUEUE_MAGIC || item == 0) return -1;
    for (;;) {
        u32 st = os_obj_lock();
        int r;
        if (q->magic != QUEUE_MAGIC || q->cap == 0u) { os_obj_unlock(st); return -1; }
        if (q->count != q->cap) {
            q_write(q, q->tail, item);
            q->tail++;
            if (q->tail == q->cap) q->tail = 0;
            q->count++;
            if (q->waiters_get != 0u) (void)os_task_wake_first(q, Q_WAIT_GET, 0);
            os_obj_unlock(st);
            return 0;
        }
        q->waiters_put++;
        r = os_task_block_prepare(q, timeout, Q_WAIT_PUT, 0u);
        os_obj_unlock(st);
        if (r != 0) { q->waiters_put--; return r; }
        r = os_task_block_wait();
        q->waiters_put--;
        if (r != 0) return r;
    }
}

int os_queue_put(OsQueue *q, void *item, u32 timeout) {
    return os_queue_put_item(q, (const void *)&item, timeout);
}

int os_queue_get_item(OsQueue *q, void *out, u32 timeout) {
    if (q == 0 || q->magic != QUEUE_MAGIC || out == 0) return -1;
    for (;;) {
        u32 st = os_obj_lock();
        int r;
        if (q->magic != QUEUE_MAGIC || q->cap == 0u) { os_obj_unlock(st); return -1; }
        if (q->count != 0u) {
            q_read(q, q->head, out);
            q->head++;
            if (q->head == q->cap) q->head = 0;
            q->count--;
            if (q->waiters_put != 0u) (void)os_task_wake_first(q, Q_WAIT_PUT, 0);
            os_obj_unlock(st);
            return 0;
        }
        q->waiters_get++;
        r = os_task_block_prepare(q, timeout, Q_WAIT_GET, 0u);
        os_obj_unlock(st);
        if (r != 0) { q->waiters_get--; return r; }
        r = os_task_block_wait();
        q->waiters_get--;
        if (r != 0) return r;
    }
}

void *os_queue_get(OsQueue *q, u32 timeout) {
    void *item = 0;
    return os_queue_get_item(q, (void *)&item, timeout) == 0 ? item : 0;
}

u32 os_queue_count(OsQueue *q) { return q == 0 ? 0u : q->count; }

/* ---- heap: 16B 块头、8B 对齐、first-fit、相邻块一次合并 ---- */
#define HDR 16u
#define BLK_MAGIC 0xB10C6E5Au
typedef struct Blk { u32 size; u32 free; u32 magic; u32 pad; } Blk;

static Blk *blk_at(u8 *p) { return (Blk *)p; }
static u8 *blk_body(Blk *b) { return (u8 *)b + HDR; }
static u8 *blk_next(Blk *b) { return (u8 *)b + HDR + b->size; }
static u32 align8(u32 x) { return (x + 7u) & ~7u; }

static int blk_ok(OsHeap *h, Blk *b) {
    u32 p = (u32)b;
    u32 end = (u32)(h->base + h->size);
    if (p < (u32)h->base || p + HDR > end || b->magic != BLK_MAGIC) return 0;
    if (b->size > end - (p + HDR)) return 0;
    return 1;
}

void os_heap_init(OsHeap *h, void *mem, u32 size) {
    u8 *base = (u8 *)mem;
    u32 off;
    Blk *b;
    if (h == 0 || mem == 0 || size < HDR + 8u) return;
    while (((u32)base & 7u) != 0u && size > 0u) { base++; size--; }
    if (size < HDR + 8u) return;
    off = (size - HDR) & ~7u;
    if (off < 8u) return;
    h->base = base; h->size = off + HDR;
    h->used = 0; h->peak = 0; h->magic = OS_HEAP_MAGIC;
    b = blk_at(base); b->size = off; b->free = 1; b->magic = BLK_MAGIC; b->pad = 0;
}

void *os_heap_alloc(OsHeap *h, u32 n) {
    u8 *p, *end;
    Blk *best = 0;
    if (h == 0 || h->magic != OS_HEAP_MAGIC || n == 0u || n > 0xFFFFFFF8u) return 0;
    n = align8(n);
    p = h->base; end = h->base + h->size;
    while (p + HDR <= end) {
        Blk *b = blk_at(p);
        if (!blk_ok(h, b)) return 0;
        if (b->free != 0u && b->size >= n) { best = b; break; }
        p = blk_next(b);
    }
    if (best == 0) return 0;
    if (best->size - n >= HDR + 8u) {
        Blk *b2 = blk_at((u8 *)best + HDR + n);
        b2->size = best->size - n - HDR; b2->free = 1; b2->magic = BLK_MAGIC; b2->pad = 0;
        best->size = n;
    }
    best->free = 0;
    h->used += best->size;
    if (h->used > h->peak) h->peak = h->used;
    return blk_body(best);
}

static void merge_next(OsHeap *h, Blk *b) {
    u8 *end = h->base + h->size;
    u8 *nq = blk_next(b);
    if (nq + HDR <= end) {
        Blk *n = blk_at(nq);
        if (blk_ok(h, n) && n->free != 0u) b->size += HDR + n->size;
    }
}

int os_heap_free(OsHeap *h, void *p) {
    u8 *q, *end;
    Blk *found = 0, *prev = 0;
    if (h == 0 || h->magic != OS_HEAP_MAGIC || p == 0) return -1;
    q = h->base; end = h->base + h->size;
    while (q + HDR <= end) {
        Blk *b = blk_at(q);
        if (!blk_ok(h, b)) return -1;
        if (blk_body(b) == (u8 *)p) { found = b; break; }
        prev = b; q = blk_next(b);
    }
    if (found == 0 || found->free != 0u || h->used < found->size) return -1;
    found->free = 1; h->used -= found->size;
    merge_next(h, found);
    if (prev != 0 && prev->free != 0u) merge_next(h, prev);
    return 0;
}

void *os_heap_realloc(OsHeap *h, void *p, u32 n) {
    Blk *b = 0;
    u8 *q, *end;
    void *np;
    u32 old, i;
    u8 *d, *s;
    if (h == 0 || h->magic != OS_HEAP_MAGIC) return 0;
    if (p == 0) return os_heap_alloc(h, n);
    if (n == 0u) { (void)os_heap_free(h, p); return 0; }
    if (n > 0xFFFFFFF8u) return 0;
    q = h->base; end = h->base + h->size;
    while (q + HDR <= end) {
        Blk *c = blk_at(q);
        if (!blk_ok(h, c)) return 0;
        if (blk_body(c) == (u8 *)p) { b = c; break; }
        q = blk_next(c);
    }
    if (b == 0 || b->free != 0u) return 0;
    old = b->size;
    if (old >= n) return p;
    np = os_heap_alloc(h, n);
    if (np == 0) return 0;
    d = (u8 *)np; s = (u8 *)p;
    for (i = 0; i < old; i++) d[i] = s[i];
    (void)os_heap_free(h, p);
    return np;
}

u32 os_heap_used(OsHeap *h) { return h == 0 || h->magic != OS_HEAP_MAGIC ? 0u : h->used; }
u32 os_heap_peak(OsHeap *h) { return h == 0 || h->magic != OS_HEAP_MAGIC ? 0u : h->peak; }
u32 os_heap_max_free(OsHeap *h) {
    u8 *p, *end; u32 best = 0;
    if (h == 0 || h->magic != OS_HEAP_MAGIC) return 0;
    p = h->base; end = h->base + h->size;
    while (p + HDR <= end) {
        Blk *b = blk_at(p);
        if (!blk_ok(h, b)) break;
        if (b->free != 0u && b->size > best) best = b->size;
        p = blk_next(b);
    }
    return best;
}

static OsHeap g_sys_heap;
void os_sys_heap_init(void *mem, u32 size) { os_heap_init(&g_sys_heap, mem, size); }
u32 os_sys_heap_used(void) { return os_heap_used(&g_sys_heap); }
u32 os_sys_heap_peak(void) { return os_heap_peak(&g_sys_heap); }
u32 os_sys_heap_max_free(void) { return os_heap_max_free(&g_sys_heap); }
