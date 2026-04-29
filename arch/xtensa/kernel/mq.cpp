/*
 * OsitoK - Message queues
 *
 * Classic bounded-buffer with two semaphores:
 *   not_full  (init = capacity) — senders wait here when full
 *   not_empty (init = 0)        — receivers wait here when empty
 *
 * The buffer critical section uses irq_save/restore since the
 * operations are just a memcpy + index bump (very fast on our
 * single-core 80 MHz processor).
 */

#include "kernel/mq.h"
#include "kernel/task.h"

extern "C" {

/* Tiny memcpy — avoids depending on ROM functions */
static void mq_copy(void *dst, const void *src, uint16_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

void mq_init(mq_t *q, void *buf, uint16_t msg_size, uint16_t capacity)
{
    q->buf = (uint8_t *)buf;
    q->msg_size = msg_size;
    q->capacity = capacity;
    q->head = 0;
    q->tail = 0;
    sem_init(&q->not_full, (int32_t)capacity);
    sem_init(&q->not_empty, 0);
}

void mq_send(mq_t *q, const void *msg)
{
    /* Wait for space */
    sem_wait(&q->not_full);

    /* Copy message into buffer (interrupts off for atomicity) */
    uint32_t ps = irq_save();
    mq_copy(&q->buf[q->head * q->msg_size], msg, q->msg_size);
    q->head = (q->head + 1) % q->capacity;
    irq_restore(ps);

    /* Signal receiver */
    sem_post(&q->not_empty);
}

void mq_recv(mq_t *q, void *msg)
{
    /* Wait for data */
    sem_wait(&q->not_empty);

    /* Copy message out of buffer */
    uint32_t ps = irq_save();
    mq_copy(msg, &q->buf[q->tail * q->msg_size], q->msg_size);
    q->tail = (q->tail + 1) % q->capacity;
    irq_restore(ps);

    /* Signal sender */
    sem_post(&q->not_full);
}

int mq_trysend(mq_t *q, const void *msg)
{
    if (sem_trywait(&q->not_full) < 0)
        return -1;

    uint32_t ps = irq_save();
    mq_copy(&q->buf[q->head * q->msg_size], msg, q->msg_size);
    q->head = (q->head + 1) % q->capacity;
    irq_restore(ps);

    sem_post(&q->not_empty);
    return 0;
}

int mq_tryrecv(mq_t *q, void *msg)
{
    if (sem_trywait(&q->not_empty) < 0)
        return -1;

    uint32_t ps = irq_save();
    mq_copy(msg, &q->buf[q->tail * q->msg_size], q->msg_size);
    q->tail = (q->tail + 1) % q->capacity;
    irq_restore(ps);

    sem_post(&q->not_full);
    return 0;
}

uint16_t mq_count(mq_t *q)
{
    int32_t items = sem_getcount(&q->not_empty);
    return (items >= 0) ? (uint16_t)items : 0;
}

} /* extern "C" */
