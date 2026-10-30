/* Isolate the device-creation parameters used by Chromium's ANGLE renderer.
 * Reuse the standalone provider probe's freestanding ABI declarations. */
#include "d3d9_provider_pe.c"

void d3d9_device_params_entry(void)
{
    UINT failures=0;
    HANDLE dll=LoadLibraryA(PROVIDER_DLL),user=LoadLibraryA("user32.dll");
    create_fn create=(void *)GetProcAddress(dll,"Direct3DCreate9");
    HRESULT (CALL *create_ex)(UINT,void **)=(void *)GetProcAddress(dll,"Direct3DCreate9Ex");
    window_fn create_window=(void *)GetProcAddress(user,"CreateWindowExA");
    destroy_fn destroy=(void *)GetProcAddress(user,"DestroyWindow");
    if (!create || !create_ex || !create_window || !destroy) {
        REPORT("D3D9PARAM FAIL exports\n");ExitProcess(2);
    }
    void *factories[2]={create(32),0};
    HRESULT factory_hr=create_ex(32,&factories[1]);
    VALUE("D3D9PARAM factory-ex-hr=",factory_hr);
    if (!factories[0] || factory_hr<0 || !factories[1]) {
        REPORT("D3D9PARAM FAIL factories\n");ExitProcess(2);
    }
    static const struct { UINT message_only,format,behavior; } cases[]={
        {0,21,0x20}, {1,21,0x20},
        {0,0,0x20},  {1,0,0x20},
        {0,21,0x856},{1,21,0x856},
        {0,0,0x856}, {1,0,0x856},
        {0,0,0x826}, {1,0,0x826}
    };
    for (UINT ex=0;ex<2;ex++) for (UINT i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        VALUE("D3D9PARAM case=",ex*16+i);
        HANDLE parent=cases[i].message_only?(HANDLE)(__INTPTR_TYPE__)-3:0;
        HANDLE window=create_window(0x08000000,"STATIC","Osito ANGLE device parameters",
            0x88000000,0,0,1,1,parent,0,0,0);
        void *device=0;
        PRESENT_PARAMETERS params={1,1,cases[i].format,1,0,0,1,window,1,0,0,0,0,0};
        HRESULT hr=window ? METHOD(factories[ex],16,device_fn)(factories[ex],0,1,window,
            cases[i].behavior,&params,&device) : (HRESULT)0x80070006;
        VALUE("D3D9PARAM device-hr=",hr);
        VALUE("D3D9PARAM resolved-format=",params.format);
        if (hr<0 || !device) { failures++;REPORT("D3D9PARAM FAIL device\n"); }
        else REPORT("D3D9PARAM PASS device\n");
        if (device) METHOD(device,2,release_fn)(device);
        if (window) destroy(window);
    }
    METHOD(factories[1],2,release_fn)(factories[1]);
    METHOD(factories[0],2,release_fn)(factories[0]);
    VALUE("D3D9PARAM DONE failures=",failures);
    ExitProcess(failures?1:0);
}
