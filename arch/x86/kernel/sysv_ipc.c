/*
 * OsitoK x86-64 — System V IPC (Shared Memory + Semaphores)
 *
 * Implements shmget/shmat/shmdt/shmctl and semaphore primitives.
 * Used by legacy apps that use System V IPC instead of POSIX shm.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void  kfree(void *ptr);

/* ── Shared Memory ───────────────────────────────────────────── */

#define SYSV_SHM_MAX  16
#define IPC_CREAT     0x0200
#define IPC_EXCL      0x0400
#define IPC_RMID      0

typedef struct {
    bool     active;
    int      key;
    uint64_t size;
    void    *addr;
    int      attach_count;
} sysv_shm_t;

static sysv_shm_t sysv_shm[SYSV_SHM_MAX];
static int sysv_shm_next_id = 1;

/* shmget: create or find shared memory segment */
int sysv_shmget(int key, uint64_t size, int flags)
{
    /* Find existing by key */
    if (key != 0 /* IPC_PRIVATE */) {
        for (int i = 0; i < SYSV_SHM_MAX; i++) {
            if (sysv_shm[i].active && sysv_shm[i].key == key) {
                if (flags & IPC_EXCL) return -17;  /* EEXIST */
                return i + 1;  /* shmid = index + 1 */
            }
        }
    }

    if (!(flags & IPC_CREAT) && key != 0) return -2;  /* ENOENT */

    /* Allocate new */
    for (int i = 0; i < SYSV_SHM_MAX; i++) {
        if (!sysv_shm[i].active) {
            uint64_t alloc_size = (size + 4095) & ~4095ULL;
            void *addr = mem_alloc_aligned(alloc_size, 4096);
            if (!addr) return -12;  /* ENOMEM */
            memset(addr, 0, alloc_size);

            sysv_shm[i].active = true;
            sysv_shm[i].key = key;
            sysv_shm[i].size = alloc_size;
            sysv_shm[i].addr = addr;
            sysv_shm[i].attach_count = 0;
            return i + 1;
        }
    }
    return -28;  /* ENOSPC */
}

/* shmat: attach shared memory to process address space */
void *sysv_shmat(int shmid, const void *shmaddr, int flags)
{
    (void)shmaddr; (void)flags;
    int idx = shmid - 1;
    if (idx < 0 || idx >= SYSV_SHM_MAX || !sysv_shm[idx].active)
        return (void *)-1;
    sysv_shm[idx].attach_count++;
    return sysv_shm[idx].addr;  /* Identity-mapped: phys == virt */
}

/* shmdt: detach shared memory */
int sysv_shmdt(const void *shmaddr)
{
    for (int i = 0; i < SYSV_SHM_MAX; i++) {
        if (sysv_shm[i].active && sysv_shm[i].addr == shmaddr) {
            if (sysv_shm[i].attach_count > 0)
                sysv_shm[i].attach_count--;
            return 0;
        }
    }
    return -22;  /* EINVAL */
}

/* shmctl: control operations (IPC_RMID = delete) */
int sysv_shmctl(int shmid, int cmd, void *buf)
{
    (void)buf;
    int idx = shmid - 1;
    if (idx < 0 || idx >= SYSV_SHM_MAX || !sysv_shm[idx].active)
        return -22;

    if (cmd == IPC_RMID) {
        if (sysv_shm[idx].attach_count == 0) {
            kfree(sysv_shm[idx].addr);
            sysv_shm[idx].active = false;
        }
        /* If still attached, mark for deletion when detach_count reaches 0 */
    }
    return 0;
}

/* ── Semaphores (simple counting) ────────────────────────────── */

#define SYSV_SEM_MAX 16

typedef struct {
    bool     active;
    int      key;
    int      value;    /* Semaphore counter */
    int      nsems;    /* Number of semaphores in set */
} sysv_sem_t;

static sysv_sem_t sysv_sem[SYSV_SEM_MAX];

/* semget: create or find semaphore set */
int sysv_semget(int key, int nsems, int flags)
{
    if (key != 0) {
        for (int i = 0; i < SYSV_SEM_MAX; i++) {
            if (sysv_sem[i].active && sysv_sem[i].key == key)
                return i + 1;
        }
    }

    if (!(flags & IPC_CREAT) && key != 0) return -2;

    for (int i = 0; i < SYSV_SEM_MAX; i++) {
        if (!sysv_sem[i].active) {
            sysv_sem[i].active = true;
            sysv_sem[i].key = key;
            sysv_sem[i].value = 0;
            sysv_sem[i].nsems = nsems;
            return i + 1;
        }
    }
    return -28;
}

/* semop: perform semaphore operation (simplified) */
int sysv_semop(int semid, void *sops, uint32_t nsops)
{
    (void)sops; (void)nsops;
    int idx = semid - 1;
    if (idx < 0 || idx >= SYSV_SEM_MAX || !sysv_sem[idx].active)
        return -22;
    /* Simplified: just return success (real impl needs blocking) */
    return 0;
}

/* semctl: control semaphore */
int sysv_semctl(int semid, int semnum, int cmd)
{
    (void)semnum;
    int idx = semid - 1;
    if (idx < 0 || idx >= SYSV_SEM_MAX || !sysv_sem[idx].active)
        return -22;

    if (cmd == IPC_RMID) {
        sysv_sem[idx].active = false;
    }
    return 0;
}
