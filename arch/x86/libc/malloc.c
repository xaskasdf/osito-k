#include <stddef.h>

extern void *sbrk(long increment);
extern void *memset(void *s, int c, size_t n);
extern void *memcpy(void *dest, const void *src, size_t n);

typedef struct block_meta {
    size_t size;
    struct block_meta *next;
    struct block_meta *prev;
    int free;
} block_meta;

typedef struct aligned_meta {
    size_t magic;
    void *raw;
    size_t requested_size;
    size_t alignment;
} aligned_meta;

#define META_SIZE sizeof(block_meta)
#define ALIGNMENT 16UL
#define ALIGN16(x) (((x) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))
#define PAGE_SIZE 65536UL
#define ALIGNED_MAGIC ((size_t)0x4F5349544F414C4EULL)
#define EINVAL_VALUE 22
#define ENOMEM_VALUE 12

static block_meta *global_base;
static block_meta *global_tail;
static block_meta *search_rover;
static volatile unsigned int allocator_lock;

static void heap_lock(void)
{
    while (__atomic_exchange_n(&allocator_lock, 1u, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&allocator_lock, __ATOMIC_RELAXED))
            __asm__ volatile ("pause");
    }
}

static void heap_unlock(void)
{
    __atomic_store_n(&allocator_lock, 0u, __ATOMIC_RELEASE);
}

static aligned_meta *aligned_header(void *ptr)
{
    if (!ptr)
        return NULL;

    aligned_meta *header = (aligned_meta *)((char *)ptr - sizeof(*header));
    if (header->magic != ALIGNED_MAGIC || !header->raw ||
        header->alignment < ALIGNMENT ||
        (header->alignment & (header->alignment - 1)) != 0)
        return NULL;

    size_t distance = (size_t)((char *)ptr - (char *)header->raw);
    if (distance < sizeof(*header) ||
        distance > sizeof(*header) + header->alignment - 1 ||
        ((size_t)ptr & (header->alignment - 1)) != 0)
        return NULL;

    return header;
}

static int blocks_are_adjacent(const block_meta *left,
                               const block_meta *right)
{
    return (const char *)(left + 1) + left->size == (const char *)right;
}

static block_meta *find_free_block(size_t size)
{
    block_meta *start = search_rover ? search_rover : global_base;
    block_meta *current = start;

    if (!current)
        return NULL;

    do {
        if (current->free && current->size >= size)
            return current;
        current = current->next ? current->next : global_base;
    } while (current != start);

    return NULL;
}

static block_meta *request_space(size_t size)
{
    if (size > (size_t)-1 - META_SIZE)
        return NULL;

    size_t request_size = size + META_SIZE;
    if (request_size < PAGE_SIZE)
        request_size = PAGE_SIZE;
    if (request_size > (size_t)-1 - (ALIGNMENT - 1))
        return NULL;
    request_size = ALIGN16(request_size);

    void *request = sbrk((long)request_size);
    if (request == (void *)-1)
        return NULL;

    block_meta *block = (block_meta *)request;
    block->size = request_size - META_SIZE;
    block->next = NULL;
    block->prev = global_tail;
    block->free = 0;

    if (global_tail)
        global_tail->next = block;
    else
        global_base = block;
    global_tail = block;

    return block;
}

static void split_block(block_meta *block, size_t size)
{
    if (block->size < size + META_SIZE + ALIGNMENT)
        return;

    block_meta *remainder =
        (block_meta *)((char *)(block + 1) + size);
    remainder->size = block->size - size - META_SIZE;
    remainder->next = block->next;
    remainder->prev = block;
    remainder->free = 1;

    if (remainder->next)
        remainder->next->prev = remainder;
    else
        global_tail = remainder;

    block->size = size;
    block->next = remainder;
}

static block_meta *merge_with_next(block_meta *block)
{
    block_meta *next = block->next;
    if (!next || !next->free || !blocks_are_adjacent(block, next))
        return block;

    if (search_rover == next)
        search_rover = block;

    block->size += META_SIZE + next->size;
    block->next = next->next;
    if (block->next)
        block->next->prev = block;
    else
        global_tail = block;

    return block;
}

static void *malloc_unlocked(size_t size)
{
    if (size == 0 || size > (size_t)-1 - (ALIGNMENT - 1))
        return NULL;

    size = ALIGN16(size);
    block_meta *block = find_free_block(size);
    if (!block) {
        block = request_space(size);
        if (!block)
            return NULL;
    }

    block->free = 0;
    split_block(block, size);
    search_rover = block->next ? block->next : global_base;
    return block + 1;
}

static void free_unlocked(void *ptr)
{
    if (!ptr)
        return;

    block_meta *block = (block_meta *)ptr - 1;
    if (block->free)
        return;

    block->free = 1;
    block = merge_with_next(block);
    if (block->prev && block->prev->free &&
        blocks_are_adjacent(block->prev, block)) {
        block = merge_with_next(block->prev);
    }
    search_rover = block;
}

void *malloc(size_t size)
{
    heap_lock();
    void *ptr = malloc_unlocked(size);
    heap_unlock();
    return ptr;
}

void free(void *ptr)
{
    if (!ptr)
        return;

    heap_lock();
    aligned_meta *header = aligned_header(ptr);
    if (header) {
        ptr = header->raw;
        header->magic = 0;
    }
    free_unlocked(ptr);
    heap_unlock();
}

int posix_memalign(void **out, size_t alignment, size_t size)
{
    if (!out || alignment < sizeof(void *) ||
        (alignment & (alignment - 1)) != 0)
        return EINVAL_VALUE;

    size_t alloc_size = size ? size : 1;
    if (alignment <= ALIGNMENT) {
        void *ptr = malloc(alloc_size);
        if (!ptr)
            return ENOMEM_VALUE;
        *out = ptr;
        return 0;
    }

    if (alloc_size > (size_t)-1 - sizeof(aligned_meta) - (alignment - 1))
        return ENOMEM_VALUE;

    void *raw = malloc(alloc_size + sizeof(aligned_meta) + alignment - 1);
    if (!raw)
        return ENOMEM_VALUE;

    size_t aligned_addr =
        ((size_t)raw + sizeof(aligned_meta) + alignment - 1) &
        ~(alignment - 1);
    aligned_meta *header =
        (aligned_meta *)(aligned_addr - sizeof(aligned_meta));
    header->raw = raw;
    header->requested_size = size;
    header->alignment = alignment;
    header->magic = ALIGNED_MAGIC;
    *out = (void *)aligned_addr;
    return 0;
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr)
        return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    if (size > (size_t)-1 - (ALIGNMENT - 1))
        return NULL;

    aligned_meta *aligned = aligned_header(ptr);
    if (aligned) {
        size_t old_size = aligned->requested_size;
        size_t alignment = aligned->alignment;
        void *new_ptr = NULL;
        if (posix_memalign(&new_ptr, alignment, size) != 0)
            return NULL;
        memcpy(new_ptr, ptr, old_size < size ? old_size : size);
        free(ptr);
        return new_ptr;
    }

    size = ALIGN16(size);
    heap_lock();

    block_meta *block = (block_meta *)ptr - 1;
    size_t old_size = block->size;
    if (old_size >= size) {
        split_block(block, size);
        if (block->next && block->next->free)
            merge_with_next(block->next);
        search_rover = block->next ? block->next : global_base;
        heap_unlock();
        return ptr;
    }

    if (block->next && block->next->free &&
        blocks_are_adjacent(block, block->next) &&
        old_size + META_SIZE + block->next->size >= size) {
        merge_with_next(block);
        split_block(block, size);
        search_rover = block->next ? block->next : global_base;
        heap_unlock();
        return ptr;
    }

    void *new_ptr = malloc_unlocked(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, old_size);
        free_unlocked(ptr);
    }

    heap_unlock();
    return new_ptr;
}

void *calloc(size_t nelem, size_t elsize)
{
    if (elsize && nelem > (size_t)-1 / elsize)
        return NULL;

    size_t size = nelem * elsize;
    void *ptr = malloc(size);
    if (ptr)
        memset(ptr, 0, size);
    return ptr;
}
