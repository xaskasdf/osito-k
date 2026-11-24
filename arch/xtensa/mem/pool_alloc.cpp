/*
 * OsitoK - Fixed-size block pool allocator
 *
 * Simple O(1) allocator using a free list.
 *   - 256 blocks of 32 bytes = 8KB total
 *   - Free list: each free block's first 4 bytes point to the next free block
 *   - Thread-safe: interrupts disabled during alloc/free
 */

#include "mem/pool_alloc.h"
#include "drivers/uart.h"

extern "C" {

/* Pool memory (8KB, aligned) */
static uint8_t pool_memory[POOL_TOTAL_SIZE] __attribute__((aligned(4)));

/* Free list head */
static void *free_list = NULL;

/* Statistics */
static uint32_t free_count = 0;
static uint32_t used_count = 0;

void pool_init(void)
{
    /* Initialize free list: chain all blocks together */
    free_list = NULL;
    free_count = POOL_NUM_BLOCKS;
    used_count = 0;

    for (int i = POOL_NUM_BLOCKS - 1; i >= 0; i--) {
        void *block = &pool_memory[i * POOL_BLOCK_SIZE];
        /* Store pointer to current free_list head in first 4 bytes */
        *(void **)block = free_list;
        free_list = block;
    }

    uart_puts("pool: initialized ");
    uart_put_dec(POOL_NUM_BLOCKS);
    uart_puts(" blocks x ");
    uart_put_dec(POOL_BLOCK_SIZE);
    uart_puts(" bytes = ");
    uart_put_dec(POOL_TOTAL_SIZE);
    uart_puts(" bytes\n");
}

void *pool_alloc(void)
{
    uint32_t ps = irq_save();

    if (free_list == NULL) {
        irq_restore(ps);
        return NULL;
    }

    /* Pop from free list */
    void *block = free_list;
    free_list = *(void **)block;

    free_count--;
    used_count++;

    irq_restore(ps);

    /* Zero the block before returning */
    ets_memset(block, 0, POOL_BLOCK_SIZE);
    return block;
}

void pool_free(void *ptr)
{
    if (ptr == NULL) return;

    /* Basic bounds check */
    uint32_t addr = (uint32_t)ptr;
    uint32_t pool_start = (uint32_t)pool_memory;
    uint32_t pool_end = pool_start + POOL_TOTAL_SIZE;

    if (addr < pool_start || addr >= pool_end) {
        ets_printf("pool: free() invalid pointer 0x%08x\n", addr);
        return;
    }

    uint32_t ps = irq_save();

    /* Push onto free list */
    *(void **)ptr = free_list;
    free_list = ptr;

    free_count++;
    used_count--;

    irq_restore(ps);
}

uint32_t pool_free_count(void)
{
    return free_count;
}

uint32_t pool_used_count(void)
{
    return used_count;
}

} /* extern "C" */
