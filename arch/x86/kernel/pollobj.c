/*
 * pollobj.c — Implementación de eventfd / timerfd / epoll / socketpair.
 *
 * Ver include/pollobj.h. Pensado para Chromium en modo --single-process:
 * los objetos viven en el proceso, sin compartirse entre procesos.
 *
 * Sobre el bloqueo: el kernel de OsitoK no tiene wait queues; el patrón que ya
 * usa sys_poll es `sti; hlt; cli` en un loop. Se usa el mismo acá para que los
 * read/write bloqueantes de eventfd/socketpair no quemen CPU.
 */

#include "../include/pollobj.h"

#include <stddef.h>

/* Errores (los defines viven en kernel/syscall.c; acá van los que usamos) */
#define EINVAL  22
#define EAGAIN  11
#define EPIPE   32

extern void *kmalloc(size_t size);
extern void kfree(void *ptr);
extern int64_t idt_get_monotonic_ns(void);
extern uint64_t idt_get_ticks(void);

/* ── ciclo de vida ─────────────────────────────────────────────── */

static ositok_poll_obj_t g_opoll[OPOLL_MAX_OBJS];

ositok_poll_obj_t *opoll_alloc(uint8_t kind)
{
    for (int i = 0; i < OPOLL_MAX_OBJS; i++) {
        if (!g_opoll[i].in_use) {
            memset(&g_opoll[i], 0, sizeof(g_opoll[i]));
            g_opoll[i].in_use = true;
            g_opoll[i].kind = kind;
            g_opoll[i].refs = 1;
            return &g_opoll[i];
        }
    }
    return NULL;
}

void opoll_ref(ositok_poll_obj_t *o)
{
    if (o) o->refs++;
}

void opoll_release(ositok_poll_obj_t *o)
{
    if (!o) return;
    if (--o->refs > 0) return;

    if (o->kind == OPOLL_SOCKPAIR && o->peer) {
        /* El peer sobrevive pero pierde referencia: EOF para el otro lado. */
        o->peer->peer = NULL;
        o->peer->sclosed = true;
        opoll_release(o->peer);
        o->peer = NULL;
    }
    o->in_use = false;
}

/* ── tiempo ────────────────────────────────────────────────────── */

uint64_t opoll_now_ns(void)
{
    return (uint64_t)idt_get_monotonic_ns();
}

/* ── readiness ─────────────────────────────────────────────────── */

bool opoll_read_ready(ositok_poll_obj_t *o)
{
    if (!o) return false;
    switch (o->kind) {
    case OPOLL_EVENTFD:
        return o->counter > 0;
    case OPOLL_TIMERFD:
        if (o->next_ns && opoll_now_ns() >= o->next_ns)
            return true;
        return o->expirations > 0;
    case OPOLL_SOCKPAIR:
        return o->scount > 0 || o->peer == NULL || o->sclosed;
    default:
        return false;
    }
}

bool opoll_write_ready(ositok_poll_obj_t *o)
{
    if (!o) return false;
    switch (o->kind) {
    case OPOLL_EVENTFD:
        /* eventfd: write siempre que no desborde (se satisface con UINT64_MAX) */
        return o->counter != 0xFFFFFFFFFFFFFFFFULL;
    case OPOLL_SOCKPAIR:
        return o->peer != NULL && !o->sclosed &&
               o->scount < OPOLL_SBUF_SIZE;
    default:
        return false;
    }
}

/* ── I/O ───────────────────────────────────────────────────────── */

int64_t opoll_read(ositok_poll_obj_t *o, void *buf, size_t count,
                   uint32_t oflags)
{
    if (!o) return -EINVAL;
    bool nonblock = (oflags & 04000 /* O_NONBLOCK */) != 0;

    switch (o->kind) {
    case OPOLL_EVENTFD: {
        if (count < 8) return -EINVAL;
        while (o->counter == 0) {
            if (nonblock) return -EAGAIN;
            __asm__ volatile ("sti; hlt; cli" ::: "memory");
        }
        uint64_t val;
        if (o->semaphore) {
            val = 1;
            o->counter--;
        } else {
            val = o->counter;
            o->counter = 0;
        }
        memcpy(buf, &val, 8);
        return 8;
    }

    case OPOLL_TIMERFD: {
        if (count < 8) return -EINVAL;
        for (;;) {
            uint64_t now = opoll_now_ns();
            if (o->next_ns && now >= o->next_ns) {
                /* Contar vencimientos (puede haber más de uno si el lector
                 * llegó tarde y hay intervalo). */
                uint64_t fired = 1;
                if (o->interval_ns) {
                    uint64_t behind = (now - o->next_ns) / o->interval_ns;
                    fired += behind;
                    o->next_ns += (behind + 1) * o->interval_ns;
                } else {
                    o->next_ns = 0;   /* one-shot: disarmado */
                }
                o->expirations = fired;
                memcpy(buf, &o->expirations, 8);
                o->expirations = 0;
                return 8;
            }
            if (nonblock) return -EAGAIN;
            __asm__ volatile ("sti; hlt; cli" ::: "memory");
        }
    }

    case OPOLL_SOCKPAIR: {
        for (;;) {
            if (o->scount > 0) {
                uint32_t n = (uint32_t)count;
                if (n > o->scount) n = o->scount;
                for (uint32_t i = 0; i < n; i++) {
                    ((uint8_t *)buf)[i] = o->sbuf[o->stail];
                    o->stail = (o->stail + 1) % OPOLL_SBUF_SIZE;
                }
                o->scount -= n;
                return (int64_t)n;
            }
            /* Sin peer o peer cerrado => EOF */
            if (!o->peer || o->sclosed) return 0;
            if (nonblock) return -EAGAIN;
            __asm__ volatile ("sti; hlt; cli" ::: "memory");
        }
    }

    default:
        return -EINVAL;
    }
}

int64_t opoll_write(ositok_poll_obj_t *o, const void *buf, size_t count,
                    uint32_t oflags)
{
    if (!o) return -EINVAL;
    bool nonblock = (oflags & 04000 /* O_NONBLOCK */) != 0;

    switch (o->kind) {
    case OPOLL_EVENTFD: {
        if (count < 8) return -EINVAL;
        uint64_t val;
        memcpy(&val, buf, 8);
        if (val == 0xFFFFFFFFFFFFFFFFULL) return -EINVAL;
        uint64_t sum = o->counter + val;
        if (sum < o->counter) return -EAGAIN;   /* overflow */
        o->counter = sum;
        return 8;
    }

    case OPOLL_SOCKPAIR: {
        if (!o->peer || o->sclosed) return -EPIPE;
        ositok_poll_obj_t *dst = o->peer;
        uint64_t written = 0;
        const uint8_t *src = (const uint8_t *)buf;
        while (written < count) {
            if (dst->scount >= OPOLL_SBUF_SIZE) {
                if (written > 0) return (int64_t)written;
                if (nonblock) return -EAGAIN;
                __asm__ volatile ("sti; hlt; cli" ::: "memory");
                if (!o->peer || o->sclosed) return -EPIPE;
                dst = o->peer;
                continue;
            }
            uint32_t space = OPOLL_SBUF_SIZE - dst->scount;
            uint32_t contig = OPOLL_SBUF_SIZE - dst->shead;
            uint32_t remain = (uint32_t)(count - written);
            uint32_t chunk = space < contig ? space : contig;
            if (chunk > remain) chunk = remain;
            memcpy(&dst->sbuf[dst->shead], src + written, chunk);
            dst->shead = (dst->shead + chunk) % OPOLL_SBUF_SIZE;
            dst->scount += chunk;
            written += chunk;
        }
        return (int64_t)written;
    }

    default:
        return -EINVAL;
    }
}
