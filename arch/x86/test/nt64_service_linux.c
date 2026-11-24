/* Verify that adding the NT namespace leaves native ELF argument dispatch intact. */
static long call(long number,long a,long b,long c,long d,long e,long f)
{
    register long r10 __asm__("r10")=d;
    register long r8 __asm__("r8")=e;
    register long r9 __asm__("r9")=f;
    long result;
    __asm__ volatile("syscall":"=a"(result):"a"(number),"D"(a),"S"(b),"d"(c),
                     "r"(r10),"r"(r8),"r"(r9):"rcx","r11","cc","memory");
    return result;
}
void _start(void)
{
    int ok=call(39,0,0,0,0,0,0)>0&&call(186,0,0,0,0,0,0)>0;
    long memory=call(9,0,4096,3,0x22,-1,0);
    ok=ok&&memory>0;
    if(memory>0){
        *(volatile long *)memory=0x12345678;
        for(int i=0;i<128;i++)ok=ok&&call(24,0,0,0,0,0,0)==0;
        ok=ok&&*(volatile long *)memory==0x12345678;
        ok=ok&&call(10,memory,4096,1,0,0,0)==0;
        ok=ok&&call(11,memory,4096,0,0,0,0)==0;
    }
    ok=ok&&call(0x7e7e7e7e,0,0,0,0,0,0)==-38;
    const char pass[]="NT64-ELF PASS\n",fail[]="NT64-ELF FAIL\n";
    call(1,1,(long)(ok?pass:fail),sizeof(pass)-1,0,0,0);
    call(60,ok?0:1,0,0,0,0,0);
    for(;;){}
}
