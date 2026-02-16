/*
 * OsitoK - Message queues (inter-task communication)
 *
 * Fixed-size message FIFO with blocking send/receive.
 * Uses semaphores for producer/consumer synchronization.
 *
 * Usage:
 *   uint8_t buf[4 * sizeof(my_msg_t)];
 *   mq_t q;
 *   mq_init(&q, buf, sizeof(my_msg_t), 4);
 *
 *   mq_send(&q, &msg);      // blocks if full
 *   mq_recv(&q, &msg);      // blocks if empty
 */
#ifndef OSITO_MQ_H
#define OSITO_MQ_H

#include "osito.h"
#include "kernel/sem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *buf;                   /* Circular buffer (caller-provided) */
    uint16_t msg_size;              /* Size of each message in bytes */
    uint16_t capacity;              /* Max number of messages */
    volatile uint16_t head;         /* Write index */
    volatile uint16_t tail;         /* Read index */
    sem_t    not_full;              /* Counts free slots (senders wait) */
    sem_t    not_empty;             /* Counts used slots (receivers wait) */
} mq_t;

/* Initialize a message queue.
 *   buf:      storage, must be at least msg_size * capacity bytes
 *   msg_size: size of one message in bytes
 *   capacity: max messages the queue can hold
 */
void mq_init(mq_t *q, void *buf, uint16_t msg_size, uint16_t capacity);

/* Send a message. Blocks if the queue is full. */
void mq_send(mq_t *q, const void *msg);

/* Receive a message. Blocks if the queue is empty. */
void mq_recv(mq_t *q, void *msg);

/* Non-blocking send. Returns 0 on success, -1 if full. */
int mq_trysend(mq_t *q, const void *msg);

/* Non-blocking receive. Returns 0 on success, -1 if empty. */
int mq_tryrecv(mq_t *q, void *msg);

/* Number of messages currently in the queue. */
uint16_t mq_count(mq_t *q);

#ifdef __cplusplus
}
#endif

#endif /* OSITO_MQ_H */
