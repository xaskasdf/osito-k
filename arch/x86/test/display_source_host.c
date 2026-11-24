#include "../kernel/display_source.c"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>
static unsigned checks;
#define CHECK(x) do { __atomic_add_fetch(&checks,1,__ATOMIC_RELAXED);if (!(x)) { fprintf(stderr,"FAIL source %d: %s\n",__LINE__,#x);abort(); } } while (0)
static void *race(void *arg)
{
    display_source_mode_t mode={(void *)(uintptr_t)(0x100000+(uintptr_t)arg*0x1000),320,200,320};
    for (unsigned i=0;i<2000;i++) {
        display_source_change_t a={0},b={0};display_source_owner_t owner={0};
        while (!display_source_begin(&a,NULL)) sched_yield();
        CHECK(display_source_adopt(&a,&owner,&mode));
        CHECK(!display_source_begin(&b,NULL));
        display_source_mode_t got={0};CHECK(display_source_mode(&owner,&got) && source_mode_equal(&got,&mode));
        CHECK(display_source_begin(&b,&mode));CHECK(!display_source_release(&owner));
        CHECK(display_source_end(&b));CHECK(display_source_release(&owner));
    }
    return NULL;
}
int main(void)
{
    display_source_change_t a={0},b={0};display_source_owner_t owner={0},other={0};
    display_source_mode_t mode={(void *)0x100000,1920,1080,1920},bad=mode,out={0};
    CHECK(!display_source_begin(NULL,NULL));CHECK(!display_source_end(&a));
    CHECK(!display_source_release(&owner));CHECK(!display_source_mode(&owner,&out));
    bad.width=0;CHECK(!display_source_begin(&a,&bad));
    CHECK(display_source_begin(&a,NULL));
    display_source_change_t copied=a;
    CHECK(!display_source_end(&copied));CHECK(!display_source_begin(&b,NULL));
    CHECK(!display_source_adopt(&a,&owner,&bad));CHECK(display_source_adopt(&a,&owner,&mode));
    CHECK(!display_source_end(&a));CHECK(!display_source_release(&other));
    display_source_owner_t cloned=owner;
    CHECK(!display_source_release(&cloned));CHECK(!display_source_mode(&cloned,&out));
    uint64_t serial=owner.serial;owner.serial++;
    CHECK(!display_source_release(&owner));CHECK(!display_source_mode(&owner,&out));owner.serial=serial;
    for (unsigned field=0;field<4;field++) {
        bad=mode;
        if (!field) bad.base=(void *)0x200000;
        if (field==1) bad.width--;
        if (field==2) bad.height--;
        if (field==3) bad.pitch++;
        CHECK(!display_source_begin(&b,&bad));CHECK(!b.self);
    }
    CHECK(display_source_begin(&b,&mode));CHECK(!display_source_release(&owner));
    CHECK(!display_source_adopt(&b,&other,&mode));CHECK(display_source_end(&b));
    CHECK(display_source_release(&owner));CHECK(display_source_begin(&a,NULL));
    CHECK(display_source_adopt(&a,&owner,&mode));
    serial=owner.serial;owner.serial=cloned.serial;
    CHECK(!display_source_release(&owner));owner.serial=serial;CHECK(display_source_release(&owner));
    pthread_t threads[4];for (uintptr_t i=0;i<4;i++) CHECK(!pthread_create(&threads[i],NULL,race,(void *)(i+1)));
    for (unsigned i=0;i<4;i++) CHECK(!pthread_join(threads[i],NULL));
    CHECK(!source_owner && !source_change);
    source_serial=UINT64_MAX-1;
    CHECK(display_source_begin(&a,NULL));CHECK(display_source_adopt(&a,&owner,&mode));
    CHECK(!display_source_begin(&b,&mode));CHECK(display_source_release(&owner));
    CHECK(!display_source_begin(&a,NULL));
    printf("Display source ownership: checks=%u failures=0\n",checks);return 0;
}
