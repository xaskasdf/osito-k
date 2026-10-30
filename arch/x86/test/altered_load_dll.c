/* Two custom dependencies distinguish application and DLL-directory search. */
#ifdef PARENT_DLL
__declspec(dllimport) int child(void);
#endif
int altered_value(void)
{
#ifdef PARENT_DLL
    return child();
#else
    static int value=VALUE;
    static int *volatile address=&value;
    return *address;
#endif
}
