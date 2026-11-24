/*
 * pollobj.h — Objetos pollables de OsitoK para Chromium.
 *
 * Chromium (y su //base) necesita el bloque de fds pollables de Linux que
 * OsitoK no tenía: eventfd, timerfd, epoll y socketpair AF_UNIX. Se implementan
 * como objetos referenciados desde fd_entry_t (campo poll_obj).
 *
 * Límites deliberadamente chicos: alcanzan para --single-process.
 */

#ifndef _OSITOK_POLLOBJ_H
#define _OSITOK_POLLOBJ_H

#include "types.h"
#include "fd.h"

/* kind de ositok_poll_obj_t */#define OPOLL_EVENTFD     1
#define OPOLL_TIMERFD     2
#define OPOLL_EPOLL       3
#define OPOLL_SOCKPAIR    4
#define OPOLL_MEMFD       5

#define OPOLL_MAX_EPOLL   32      /* fds registrados por instancia de epoll */
#define OPOLL_SBUF_SIZE   65536   /* buffer de cada dirección del socketpair */
#define OPOLL_MAX_OBJS    64
#define OPOLL_MAX_FDQ     8       /* mensajes con fds encolados (SCM_RIGHTS) */

typedef struct ositok_poll_obj {
    uint8_t  kind;
    int      refs;
    bool     in_use;

    /* eventfd */
    uint64_t counter;
    int      semaphore;          /* EFD_SEMAPHORE */

    /* timerfd */
    int      clockid;
    uint64_t interval_ns;        /* 0 = one-shot */
    uint64_t next_ns;            /* 0 = disarmado */
    uint64_t expirations;

    /* epoll */
    struct {
        int      fd;
        uint32_t events;
        uint64_t data;
    } ep[OPOLL_MAX_EPOLL];
    int      ep_count;

    /* socketpair */
    struct ositok_poll_obj *peer;
    uint8_t  sbuf[OPOLL_SBUF_SIZE];
    uint32_t shead, stail, scount;
    bool     sclosed;
    int      sotype;             /* SOCK_STREAM / SOCK_SEQPACKET */

    /* SCM_RIGHTS: fds encolados por mensaje recibido */
    int      fdq[OPOLL_MAX_FDQ][4];
    uint8_t  fdq_len[OPOLL_MAX_FDQ];
    int      fdq_count;
} ositok_poll_obj_t;

/* Ciclo de vida */
ositok_poll_obj_t *opoll_alloc(uint8_t kind);
void opoll_ref(ositok_poll_obj_t *o);
void opoll_release(ositok_poll_obj_t *o);

/* Readiness (para poll_check / epoll_wait) */
bool opoll_read_ready(ositok_poll_obj_t *o);
bool opoll_write_ready(ositok_poll_obj_t *o);

/* I/O */
int64_t opoll_read(ositok_poll_obj_t *o, void *buf, size_t count,
                   uint32_t oflags);
int64_t opoll_write(ositok_poll_obj_t *o, const void *buf, size_t count,
                    uint32_t oflags);

/* Reloj monotónico en ns (misma fuente que clock_gettime) */
uint64_t opoll_now_ns(void);

#endif /* _OSITOK_POLLOBJ_H */
