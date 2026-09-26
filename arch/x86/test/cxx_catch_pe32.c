/* Exercise the MSVC catch-object ABI independently of a C++ standard library. */
typedef unsigned int U32;
typedef unsigned char U8;
__declspec(dllimport) void __stdcall ExitProcess(U32 code);
__declspec(dllimport) void *__stdcall GetStdHandle(U32 which);
__declspec(dllimport) int __stdcall WriteFile(void *, const void *, U32, U32 *, void *);

extern void exercise_catch(void);
extern void throw_payload(void);
extern U32 catch_adjectives, catch_properties, catch_size;
extern char catch_type_name[];
typedef long (__stdcall *WindowProc)(void *, U32, U32, long);
__declspec(dllimport) long __stdcall CallWindowProcA(WindowProc, void *, U32, U32, long);
U8 catch_payload[16];
static U32 checks, failures, called;
static U32 escape_callbacks;

static void print(const char *s)
{
    U32 length=0, written=0;
    while(s[length]) length++;
    WriteFile(GetStdHandle((U32)-11),s,length,&written,0);
}
static void number(U32 value)
{
    char s[12]; U32 n=0;
    do { s[n++]=(char)('0'+value%10); value/=10; } while(value);
    while(n) { char digit[2]={s[--n],0}; print(digit); }
}
static void check(int ok,const char *label)
{
    checks++;
    if(!ok) { failures++; print("[CXX-CATCH] FAIL "); print(label); print("\n"); }
}
void verify_catch(const U8 *slot, U32 before, U32 after)
{
    called++;
    check(before==0x6ac03519 && after==0xc10357ab,"catch slot bounds");
    if(catch_adjectives & 8) {
        check(*(const U32 *)slot==(U32)catch_payload,"reference binds to thrown object");
    } else {
        int same=1;
        for(U32 i=0;i<catch_size;i++) if(slot[i]!=catch_payload[i]) same=0;
        check(same,"by-value catch copies object bytes");
    }
}
static long __stdcall throwing_proc(void *window, U32 message, U32 depth, long data)
{
    if(depth)
        CallWindowProcA(throwing_proc,window,message,depth-1,data);
    throw_payload();
    return 0;
}
void raise_from_test(void)
{
    if(escape_callbacks)
        CallWindowProcA(throwing_proc,0,0x401,escape_callbacks-1,0);
    else
        throw_payload();
}
static long __stdcall catching_proc(void *window, U32 message, U32 depth, long data)
{
    check(!window && message==0x402 && data==0x19,"callback arguments");
    if(depth)
        check(CallWindowProcA(catching_proc,window,message,depth-1,data)==0x3179,
              "nested callback returns after catch");
    exercise_catch();
    return 0x3179;
}
static void run(const char *type,U32 properties,U32 size,U32 adjectives)
{
    U32 i=0;
    do { catch_type_name[i]=type[i]; } while(type[i++]);
    catch_properties=properties; catch_size=size; catch_adjectives=adjectives;
    U32 before=called;
    exercise_catch();
    check(called==before+1,"matching handler entered once");
}
void mainCRTStartup(void)
{
    for(U32 i=0;i<sizeof(catch_payload);i++) catch_payload[i]=(U8)(0xa1+i*3);
    run(".H",1,4,0);
    run(".G",1,2,0);
    run("._J",1,8,0);
    run(".N",1,8,0);
    run(".?AUTestPod@@",0,12,0);
    run(".H",1,4,8);
    run(".?AUTestPod@@",0,12,8);
    static const unsigned short text[]={ 'p','a','c','k','a','g','e',0 };
    *(U32 *)catch_payload=(U32)text;
    run(".PBG",1,4,0);
    run(".PBG",1,4,8);
    *(U32 *)catch_payload=0;
    run(".PBG",1,4,0);
    for(U32 depth=0;depth<3;depth++) {
        U32 before=called;
        check(CallWindowProcA(catching_proc,0,0x402,depth,0x19)==0x3179,
              "callback returns after catch");
        check(called==before+depth+1,"nested callback catch count");
    }
    for(escape_callbacks=1;escape_callbacks<=3;escape_callbacks++)
        run(".PBG",1,4,0);
    escape_callbacks=0;
    run(".H",1,4,0);
    print("[CXX-CATCH] checks="); number(checks); print(" failures="); number(failures);
    print(failures?" FAIL\n":" PASS\n");
    ExitProcess(failures?3:0);
}
