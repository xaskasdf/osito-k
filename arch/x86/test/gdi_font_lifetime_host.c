/* Logical font allocation failures, identifier wrap, ownership and concurrent
 * access against production GDI, using the existing surface host adapters. */
#include "../include/types.h"
static __thread unsigned font_owner = 1;
static unsigned font_allocations, font_releases, fail_font_allocate;
static void *font_host_allocate(uint64_t n);
static void font_host_free(void *p);
#define GDI_HOST_TEST_ALLOCATE font_host_allocate
#define GDI_HOST_TEST_FREE font_host_free
#define GDI_HOST_TEST_PROCESS_ID font_owner
#define main gdi_surface_regression_unused_main
#include "gdi_surface_lifetime_host.c"
#undef main

typedef unsigned long font_pthread_t;
extern int pthread_create(font_pthread_t *, const void *, void *(*)(void *), void *);
extern int pthread_join(font_pthread_t, void **);
static unsigned font_workers_start;
typedef struct { unsigned failures, checks; } FONT_WORKER;

static void *font_host_allocate(uint64_t n)
{
    if (__atomic_exchange_n(&fail_font_allocate, 0, __ATOMIC_ACQ_REL)) return NULL;
    void *p = malloc(n);
    if (p) __atomic_add_fetch(&font_allocations, 1, __ATOMIC_RELAXED);
    return p;
}

static void font_host_free(void *p)
{
    if (p) __atomic_add_fetch(&font_releases, 1, __ATOMIC_RELAXED);
    free(p);
}

static void *font_host_worker(void *argument)
{
    FONT_WORKER *worker = argument;
    font_owner = 1;
    while (!__atomic_load_n(&font_workers_start, __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
    for (unsigned round = 0; round < 100; round++) {
        HGDIOBJ objects[8] = {0};
        GDI_LOGFONTW request = {0}, observed;
        request.lfHeight = -(LONG)(round + 10);
        request.lfWeight = 400;
        for (unsigned i = 0; i < 8; i++) {
            objects[i] = gdi_create_font(&request);
            worker->checks++;
            if (!objects[i]) worker->failures++;
            worker->checks++;
            if (!font_from_handle(objects[i], &observed, TRUE) ||
                observed.lfHeight != request.lfHeight || observed.lfWeight != 400)
                worker->failures++;
        }
        for (unsigned i = 0; i < 8; i++) {
            worker->checks++;
            if (!DeleteObject(objects[i])) worker->failures++;
        }
    }
    return NULL;
}

int main(void)
{
    GDI_LOGFONTW request = {0}, observed;
    request.lfHeight = -19;
    request.lfWidth = 9;
    request.lfWeight = 400;
    request.lfFaceName[0] = 'A';
    request.lfFaceName[1] = 'B';
    for (int abi = 0; abi < 2; abi++) {
        compat32 = abi;
        HGDIOBJ objects[512];
        for (unsigned i = 0; i < 512; i++) {
            request.lfHeight = -(LONG)(i + 10);
            objects[i] = gdi_create_font(&request);
            CHECK(objects[i]);
            CHECK(gdi_get_object(objects[i], sizeof(observed), &observed, TRUE) == sizeof(observed));
            CHECK(observed.lfHeight == request.lfHeight && observed.lfWeight == 400);
        }
        for (unsigned i = 0; i < 512; i++) CHECK(DeleteObject(objects[i]));
    }
    compat32 = 0;
    fail_font_allocate = 1;
    CHECK(!gdi_create_font(&request));
    next_font_id = 1;
    HGDIOBJ first = gdi_create_font(&request);
    next_font_id = 0xFFFFFFu;
    HGDIOBJ last = gdi_create_font(&request), wrapped = gdi_create_font(&request);
    CHECK(first && last && wrapped && first != wrapped && last != wrapped);
    CHECK(DeleteObject(last));
    CHECK(!DeleteObject(last));
    CHECK(font_from_handle(first, &observed, TRUE));
    CHECK(DeleteObject(first) && DeleteObject(wrapped));

    font_owner = 1;
    first = gdi_create_font(&request);
    HDC dc = CreateCompatibleDC(NULL);
    CHECK(first && dc && SelectObject(dc, first) == STOCK_SYSTEM_FONT);
    CHECK(SaveDC(dc) == 1 && !DeleteObject(first));
    int widths[2]; WCHAR face[3];
    CHECK(GetCharWidthW_k32(dc, 'A', 'B', widths) && widths[0] == 9 && widths[1] == 9);
    CHECK(GetTextFaceW_k32(dc, 3, face) == 2 && face[0] == 'A' && face[1] == 'B' && !face[2]);
    CHECK(SelectObject(dc, STOCK_SYSTEM_FONT) == first && !DeleteObject(first));
    CHECK(DeleteDC(dc) && DeleteObject(first));

    first = gdi_create_font(&request);
    last = gdi_create_font(&request);
    font_owner = 2;
    wrapped = gdi_create_font(&request);
    dc = CreateCompatibleDC(NULL);
    CHECK(first && last && wrapped && dc);
    CHECK(font_from_handle(first, &observed, FALSE));
    CHECK(!DeleteObject(first));
    CHECK(SelectObject(dc, first) == NULL && GetCurrentObject(dc, OBJ_FONT) == STOCK_SYSTEM_FONT);
    CHECK(SelectObject(dc, wrapped) == STOCK_SYSTEM_FONT);
    gdi32_release_process(1);
    CHECK(!font_from_handle(first, &observed, FALSE) && !font_from_handle(last, &observed, FALSE));
    CHECK(font_from_handle(wrapped, &observed, TRUE) && dc_from_handle(dc));
    gdi32_release_process(2);
    CHECK(!font_from_handle(wrapped, &observed, FALSE) && !dc_from_handle(dc));

    FONT_WORKER workers[4] = {{0}};
    font_pthread_t threads[4];
    unsigned started = 0;
    for (unsigned i = 0; i < 4; i++) {
        int result = pthread_create(&threads[i], NULL, font_host_worker, &workers[i]);
        CHECK(!result);
        if (result) break;
        started++;
    }
    __atomic_store_n(&font_workers_start, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < started; i++) {
        CHECK(!pthread_join(threads[i], NULL));
        checks += workers[i].checks;
        failures += workers[i].failures;
    }
    for (unsigned bucket = 0; bucket < FONT_BUCKETS; bucket++) CHECK(!gdi_fonts[bucket]);
    CHECK(font_allocations == font_releases);
    printf("GDI font lifetime: %u checks, %u failures; allocated=%u freed=%u\n",
           checks, failures, font_allocations, font_releases);
    return failures ? 1 : 0;
}
