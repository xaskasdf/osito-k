/*
 * OsitoK - Kernel configuration constants
 */
#ifndef OSITO_CONFIG_H
#define OSITO_CONFIG_H

/* Maximum number of tasks (including idle) */
#define MAX_TASKS       8

/* Scheduler tick rate in Hz */
#define TICK_HZ         100

/* Task stack size in bytes */
#define TASK_STACK_SIZE 1536

/* ISR stack size in bytes */
#define ISR_STACK_SIZE  512

/* CPU clock frequency */
#define CPU_FREQ_HZ     80000000UL

/* Timer prescaler for FRC1 */
#define FRC1_PRESCALER  16

/* FRC1 load value: CPU_FREQ / prescaler / TICK_HZ */
#define FRC1_LOAD_VAL   (CPU_FREQ_HZ / FRC1_PRESCALER / TICK_HZ)  /* 50000 */

/* Memory pool configuration */
#define POOL_BLOCK_SIZE 32
#define POOL_NUM_BLOCKS 256
#define POOL_TOTAL_SIZE (POOL_BLOCK_SIZE * POOL_NUM_BLOCKS)  /* 8KB */

/* Heap configuration */
#define HEAP_SIZE       8192  /* 8KB heap for variable-size allocations */

/* UART configuration */
#define UART_BAUD       115200
#define UART_RX_BUF_SIZE 64

/* Context frame size: 20 registers * 4 bytes = 80 bytes */
#define CONTEXT_FRAME_SIZE 80

/* DRAM boundaries */
#define DRAM_START      0x3FFE8000
#define DRAM_END        0x3FFFBFFF
#define STACK_TOP       0x3FFFBFF0

/* IRAM boundaries */
#define IRAM_START      0x40100000
#define IRAM_END        0x40107FFF

#endif /* OSITO_CONFIG_H */
