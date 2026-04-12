#include <stddef.h>

extern void *sbrk(long increment);
extern void *memset(void *s, int c, size_t n);
extern void *memcpy(void *dest, const void *src, size_t n);

typedef struct block_meta {
    size_t size;
    struct block_meta *next;
    int free;
} block_meta;

#define META_SIZE sizeof(block_meta)
#define ALIGN16(x) (((x) + 15) & ~15UL)
#define PAGE_SIZE 65536

static block_meta *global_base = NULL;

static block_meta *find_free_block(block_meta **last, size_t size) {
    block_meta *current = global_base;
    while (current && !(current->free && current->size >= size)) {
        *last = current;
        current = current->next;
    }
    return current;
}

static block_meta *request_space(block_meta* last, size_t size) {
    block_meta *block;
    size_t request_size = size + META_SIZE;
    if (request_size < PAGE_SIZE) {
        request_size = PAGE_SIZE;
    }
    request_size = ALIGN16(request_size);
    
    block = (block_meta *)sbrk(0);
    void *request = sbrk(request_size);
    if (request == (void*)-1) {
        return NULL;
    }
    
    if (last) {
        last->next = block;
    }
    
    block->size = request_size - META_SIZE;
    block->next = NULL;
    block->free = 0;
    
    return block;
}

static void split_block(block_meta *block, size_t size) {
    if (block->size >= size + META_SIZE + 16) {
        block_meta *new_block = (block_meta *)((char *)block + META_SIZE + size);
        new_block->size = block->size - size - META_SIZE;
        new_block->next = block->next;
        new_block->free = 1;
        
        block->size = size;
        block->next = new_block;
    }
}

static void merge_free_blocks() {
    block_meta *curr = global_base;
    while (curr && curr->next) {
        if (curr->free && curr->next->free) {
            curr->size += META_SIZE + curr->next->size;
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
}

void *malloc(size_t size) {
    block_meta *block;
    
    if (size <= 0) return NULL;
    
    size = ALIGN16(size);
    
    if (!global_base) {
        block = request_space(NULL, size);
        if (!block) return NULL;
        global_base = block;
    } else {
        block_meta *last = global_base;
        block = find_free_block(&last, size);
        if (!block) {
            block = request_space(last, size);
            if (!block) return NULL;
        } else {
            block->free = 0;
        }
    }
    
    split_block(block, size);
    return (block + 1);
}

static block_meta *get_block_ptr(void *ptr) {
    return (block_meta*)ptr - 1;
}

void free(void *ptr) {
    if (!ptr) return;
    
    block_meta* block_ptr = get_block_ptr(ptr);
    block_ptr->free = 1;
    merge_free_blocks();
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    
    size = ALIGN16(size);
    block_meta* block_ptr = get_block_ptr(ptr);
    if (block_ptr->size >= size) {
        split_block(block_ptr, size);
        return ptr;
    }
    
    void *new_ptr = malloc(size);
    if (!new_ptr) return NULL;
    
    memcpy(new_ptr, ptr, block_ptr->size);
    free(ptr);
    return new_ptr;
}

void *calloc(size_t nelem, size_t elsize) {
    size_t size = nelem * elsize;
    void *ptr = malloc(size);
    if (ptr) memset(ptr, 0, size);
    return ptr;
}
