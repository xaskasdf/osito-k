#include "load_dependency_common.h"
__declspec(dllimport) DWORD lifecycle_value(void);
__declspec(dllexport) DWORD dependency_value(void)
{
    return lifecycle_value();
}
int CALL dependency_main(void *module,DWORD reason,void *reserved)
{
    (void)module;(void)reserved;
    if(reason==1){
        char name[80];dependency_name(name);
        void *event=OpenEventA(2,0,name);
        if(event){SetEvent(event);CloseHandle(event);}
    }
    return 1;
}
