/* Exercise an external D3D9 provider without replacing the system DLL. */
typedef unsigned int DWORD, UINT;
typedef int BOOL, HRESULT;
typedef void *HANDLE;
int _fltused;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
#ifdef D3D9_SYSTEM_BRIDGE
#define PROVIDER_DLL "d3d9.dll"
#else
#define PROVIDER_DLL "d3d9-native.dll"
#endif
API void CALL ExitProcess(UINT);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API DWORD CALL GetLastError(void);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);

typedef struct {
    UINT width, height, format, count, multisample, quality, swap;
    HANDLE window;
    BOOL windowed, auto_depth;
    UINT depth_format, flags, refresh, interval;
} PRESENT_PARAMETERS;
typedef struct { int pitch; void *pixels; } LOCKED_RECT;
typedef HANDLE (CALL *window_fn)(DWORD, const char *, const char *, DWORD,
    int, int, int, int, HANDLE, HANDLE, HANDLE, void *);
typedef BOOL (CALL *destroy_fn)(HANDLE);
typedef void *(CALL *create_fn)(UINT);
typedef UINT (CALL *count_fn)(void *);
typedef DWORD (CALL *release_fn)(void *);
typedef HRESULT (CALL *device_fn)(void *, UINT, UINT, HANDLE, DWORD,
                                  PRESENT_PARAMETERS *, void **);
typedef HRESULT (CALL *clear_fn)(void *, DWORD, const void *, DWORD, DWORD, float, DWORD);
typedef HRESULT (CALL *present_fn)(void *, const void *, const void *, HANDLE, const void *);
typedef HRESULT (CALL *back_fn)(void *, UINT, UINT, UINT, void **);
typedef HRESULT (CALL *surface_fn)(void *, UINT, UINT, UINT, UINT, void **, HANDLE *);
typedef HRESULT (CALL *read_fn)(void *, void *, void *);
typedef HRESULT (CALL *lock_fn)(void *, LOCKED_RECT *, const void *, DWORD);
typedef HRESULT (CALL *unlock_fn)(void *);
typedef HRESULT (CALL *state_fn)(void *, DWORD, DWORD);
typedef HRESULT (CALL *fvf_fn)(void *, DWORD);
typedef HRESULT (CALL *scene_fn)(void *);
typedef HRESULT (CALL *draw_fn)(void *, UINT, UINT, const void *, UINT);
typedef HRESULT (CALL *shader_create_fn)(void *, const DWORD *, void **);
typedef HRESULT (CALL *shader_set_fn)(void *, void *);
#define METHOD(object, slot, type) ((type)(*(void ***)(object))[slot])
#define REPORT(text) do { static const char msg[] = text; DWORD written; \
    WriteFile(GetStdHandle((DWORD)-11), msg, sizeof(msg)-1, &written, 0); } while (0)
#define VALUE(label, value) do { DWORD number = (DWORD)(value), written; \
    char digits[10]; REPORT(label); \
    for (UINT i=0; i<8; i++) digits[i]="0123456789abcdef"[(number >> ((7-i)*4)) & 15]; \
    digits[8]='\n'; WriteFile(GetStdHandle((DWORD)-11),digits,9,&written,0); } while (0)
#define REQUIRE(test, label) do { if (!(test)) { REPORT("D3D9PROV FAIL " label "\n"); \
    VALUE("D3D9PROV error=", GetLastError()); failed=1; goto done; } \
    REPORT("D3D9PROV PASS " label "\n"); } while (0)

void d3d9_provider_entry(void)
{
    UINT failed = 0;
    void *d3d = 0, *device = 0, *back = 0, *surface = 0, *shader = 0;
    HANDLE window = 0;
    destroy_fn destroy = 0;
#ifdef D3D9_PRESSURE_WINDOWS
    static HANDLE pressure_windows[D3D9_PRESSURE_WINDOWS];
    HANDLE pressure_user = LoadLibraryA("user32.dll");
    window_fn pressure_create = (void *)GetProcAddress(pressure_user,"CreateWindowExA");
    destroy = (void *)GetProcAddress(pressure_user,"DestroyWindow");
    REQUIRE(pressure_create && destroy,"pressure-exports");
    for (UINT i=0;i<D3D9_PRESSURE_WINDOWS;++i) {
        pressure_windows[i] = pressure_create(0,"STATIC","D3D9 held surface",0x80000000,
            0,0,16,16,0,0,0,0);
        REQUIRE(pressure_windows[i],"pressure-window");
    }
    VALUE("D3D9PROV held-windows=",D3D9_PRESSURE_WINDOWS);
#endif
#ifdef D3D9_BENCHMARK
    DWORD (CALL *ticks)(void) = (void *)GetProcAddress(LoadLibraryA("kernel32.dll"),"GetTickCount");
#if defined(D3D9_CSMT_SETTING)
    HANDLE config = 0;
    BOOL config_changed = 0;
    unsigned char previous[256];
    DWORD previous_type = 0, previous_size = sizeof(previous);
    int previous_status = 2;
    HANDLE registry = LoadLibraryA("advapi32.dll");
    int (CALL *reg_create)(HANDLE,const char *,DWORD,char *,DWORD,DWORD,void *,HANDLE *,DWORD *) =
        (void *)GetProcAddress(registry,"RegCreateKeyExA");
    int (CALL *reg_query)(HANDLE,const char *,DWORD *,DWORD *,unsigned char *,DWORD *) =
        (void *)GetProcAddress(registry,"RegQueryValueExA");
    int (CALL *reg_set)(HANDLE,const char *,DWORD,DWORD,const unsigned char *,DWORD) =
        (void *)GetProcAddress(registry,"RegSetValueExA");
    int (CALL *reg_delete)(HANDLE,const char *) = (void *)GetProcAddress(registry,"RegDeleteValueA");
    int (CALL *reg_close)(HANDLE) = (void *)GetProcAddress(registry,"RegCloseKey");
    REQUIRE(reg_create && reg_query && reg_set && reg_delete && reg_close,"benchmark-registry-exports");
    REQUIRE(!reg_create((HANDLE)(__INTPTR_TYPE__)(int)0x80000001,"Software\\Wine\\Direct3D",
        0,0,0,0x20003,0,&config,0),"benchmark-registry-key");
    previous_status = reg_query(config,"csmt",0,&previous_type,previous,&previous_size);
    REQUIRE(previous_status == 0 || previous_status == 2,"benchmark-save-setting");
    DWORD setting = D3D9_CSMT_SETTING;
    REQUIRE(!reg_set(config,"csmt",0,4,(const unsigned char *)&setting,sizeof(setting)),"benchmark-set-csmt");
    config_changed = 1;
    VALUE("D3D9BENCH csmt=",setting);
#endif
    REQUIRE(ticks,"benchmark-clock");
#endif
    REPORT("D3D9PROV BEGIN\n");
    HANDLE dll = LoadLibraryA(PROVIDER_DLL);
    REQUIRE(dll, "provider-load");
    create_fn create = (create_fn)GetProcAddress(dll, "Direct3DCreate9");
    REQUIRE(create, "provider-export");
    d3d = create(32);
    REQUIRE(d3d, "factory");
    UINT adapters = METHOD(d3d, 4, count_fn)(d3d);
    VALUE("D3D9PROV adapters=", adapters);
    REQUIRE(adapters, "adapters");
    HANDLE user = LoadLibraryA("user32.dll");
    window_fn create_window = (window_fn)GetProcAddress(user, "CreateWindowExA");
    destroy = (destroy_fn)GetProcAddress(user, "DestroyWindow");
    REQUIRE(create_window && destroy, "window-exports");
    window = create_window(0, "STATIC", "D3D9 provider rendering", 0x10CF0000,
                            40,40,200,160,0,0,0,0);
    REQUIRE(window, "window");
    /* WineD3D must obtain this window's DC. Otherwise its offscreen fallback
     * can pass render/readback tests while Present never reaches the window. */
    HANDLE (CALL *get_dcex)(HANDLE,HANDLE,DWORD) = (void *)GetProcAddress(user,"GetDCEx");
    HANDLE (CALL *window_from_dc)(HANDLE) = (void *)GetProcAddress(user,"WindowFromDC");
    int (CALL *release_dc)(HANDLE,HANDLE) = (void *)GetProcAddress(user,"ReleaseDC");
    REQUIRE(get_dcex && window_from_dc && release_dc,"swapchain-dc-exports");
    HANDLE swapchain_dc=get_dcex(window,0,0x10002);
    BOOL target_matches=swapchain_dc && window_from_dc(swapchain_dc)==window;
    if (swapchain_dc) release_dc(window,swapchain_dc);
    REQUIRE(target_matches,"swapchain-dc-target");
    PRESENT_PARAMETERS params = {64,64,21,1,0,0,1,window,1,0,0,0,0,0};
    HRESULT hr = METHOD(d3d, 16, device_fn)(d3d,0,1,window,0x20,&params,&device);
    VALUE("D3D9PROV device-hr=", hr);
    REQUIRE(hr >= 0 && device, "device");
    hr = METHOD(device, 43, clear_fn)(device,0,0,1,0xff224466,1.0f,0);
    VALUE("D3D9PROV clear-hr=",hr);
    REQUIRE(hr >= 0,"clear");
    hr = METHOD(device, 18, back_fn)(device,0,0,0,&back);
    REQUIRE(hr >= 0 && back,"back-buffer");
    hr = METHOD(device, 36, surface_fn)(device,64,64,21,2,&surface,0);
    VALUE("D3D9PROV surface-hr=",hr);
    REQUIRE(hr >= 0 && surface,"readback-surface");
    hr = METHOD(device, 32, read_fn)(device,back,surface);
    VALUE("D3D9PROV readback-hr=",hr);
    REQUIRE(hr >= 0,"readback");
    LOCKED_RECT locked = {0,0};
    hr = METHOD(surface,13,lock_fn)(surface,&locked,0,0x10);
    REQUIRE(hr >= 0 && locked.pixels && locked.pitch >= 256,"lock");
    DWORD pixel = *(DWORD *)((char *)locked.pixels+32*locked.pitch+32*4);
    VALUE("D3D9PROV center-pixel=",pixel);
    hr = METHOD(surface,14,unlock_fn)(surface);
    REQUIRE(hr >= 0 && pixel == 0xff224466,"rendered-color");
    struct { float x,y,z,w; DWORD color; } vertices[3] = {
        {0,64,0.5f,1,0xff00ff00}, {32,0,0.5f,1,0xff00ff00},
        {64,64,0.5f,1,0xff00ff00}
    };
    /* ps_2_0: def c0, 0, 0, 1, 1; mov oC0, c0. */
    static const DWORD blue_shader[] = {
        0xffff0200,0x05000051,0xa00f0000,0,0,0x3f800000,0x3f800000,
        0x02000001,0x800f0800,0xa0e40000,0x0000ffff
    };
    REQUIRE(METHOD(device,57,state_fn)(device,137,0) >= 0,"disable-lighting");
    REQUIRE(METHOD(device,57,state_fn)(device,22,1) >= 0,"disable-culling");
    REQUIRE(METHOD(device,57,state_fn)(device,7,0) >= 0,"disable-depth");
    REQUIRE(METHOD(device,89,fvf_fn)(device,0x44) >= 0,"transformed-color-fvf");
    for (UINT phase=0; phase<2; ++phase) {
        if (phase) {
            hr=METHOD(device,106,shader_create_fn)(device,blue_shader,&shader);
            REQUIRE(hr >= 0 && shader,"create-pixel-shader");
            REQUIRE(METHOD(device,107,shader_set_fn)(device,shader) >= 0,"bind-pixel-shader");
        }
        REQUIRE(METHOD(device,43,clear_fn)(device,0,0,1,0xff000000,1.0f,0) >= 0,"draw-clear");
        REQUIRE(METHOD(device,41,scene_fn)(device) >= 0,"begin-scene");
        hr=METHOD(device,83,draw_fn)(device,4,1,vertices,sizeof(vertices[0]));
        REQUIRE(hr >= 0,"draw-triangle");
        REQUIRE(METHOD(device,42,scene_fn)(device) >= 0,"end-scene");
        REQUIRE(METHOD(device,32,read_fn)(device,back,surface) >= 0,"draw-readback");
        REQUIRE(METHOD(surface,13,lock_fn)(surface,&locked,0,0x10) >= 0,"draw-lock");
        pixel=*(DWORD *)((char *)locked.pixels+32*locked.pitch+32*4);
        VALUE("D3D9PROV draw-pixel=",pixel);
        hr=METHOD(surface,14,unlock_fn)(surface);
        REQUIRE(hr >= 0 && pixel == (phase ? 0xff0000ff : 0xff00ff00),"triangle-color");
    }
    hr = METHOD(device,17,present_fn)(device,0,0,0,0);
    VALUE("D3D9PROV present-hr=",hr);
    REQUIRE(hr >= 0,"present");
#ifdef D3D9_BENCHMARK
    /* Warmup above is excluded. The same 16 frames alternate fixed-function
     * and shader rendering, force readback, validate pixels, then present.
     * No successful-check logging is included in the measured interval. */
    DWORD started = ticks();
    for (UINT frame=0; frame<16; ++frame) {
        DWORD expected = (frame & 1) ? 0xff0000ff : 0xff00ff00;
        if (METHOD(device,107,shader_set_fn)(device,(frame & 1) ? shader : 0) < 0 ||
            METHOD(device,43,clear_fn)(device,0,0,1,0xff000000,1.0f,0) < 0 ||
            METHOD(device,41,scene_fn)(device) < 0 ||
            METHOD(device,83,draw_fn)(device,4,1,vertices,sizeof(vertices[0])) < 0 ||
            METHOD(device,42,scene_fn)(device) < 0 ||
            METHOD(device,32,read_fn)(device,back,surface) < 0 ||
            METHOD(surface,13,lock_fn)(surface,&locked,0,0x10) < 0) {
            failed=1; REPORT("D3D9BENCH FAIL frame-operations\n"); goto done;
        }
        BOOL mapped = locked.pixels && locked.pitch >= 256;
        pixel = mapped ? *(DWORD *)((char *)locked.pixels+32*locked.pitch+32*4) : 0;
        DWORD corner = mapped ? *(DWORD *)((char *)locked.pixels+2*locked.pitch+2*4) : 0;
        hr = METHOD(surface,14,unlock_fn)(surface);
        if (hr < 0 || pixel != expected || corner != 0xff000000 ||
            METHOD(device,17,present_fn)(device,0,0,0,0) < 0) {
            failed=1; REPORT("D3D9BENCH FAIL frame-pixels-present\n");
            VALUE("D3D9BENCH frame=",frame); VALUE("D3D9BENCH pixel=",pixel);
            VALUE("D3D9BENCH corner=",corner); goto done;
        }
    }
    DWORD elapsed = ticks()-started;
    VALUE("D3D9BENCH frames=",16);
    VALUE("D3D9BENCH elapsed-ms=",elapsed);
    REPORT("D3D9BENCH PASS alternating-pixels\n");
#endif
    /* Optional observation interval for a VM framebuffer capture. It does not
     * alter the rendering contract exercised by the native oracle. */
    const char *(CALL *command_line)(void)=(void *)GetProcAddress(LoadLibraryA("kernel32.dll"),"GetCommandLineA");
    const char *arg=command_line ? command_line() : "";
    for (; *arg; ++arg) if (arg[0]=='-' && arg[1]=='-' && arg[2]=='h' &&
        arg[3]=='o' && arg[4]=='l' && arg[5]=='d' && (!arg[6] || arg[6]==' ')) {
        void (CALL *sleep_ms)(DWORD)=(void *)GetProcAddress(LoadLibraryA("kernel32.dll"),"Sleep");
        REQUIRE(sleep_ms,"observation-sleep");
        REPORT("D3D9PROV OBSERVE\n");sleep_ms(30000);break;
    }
done:
    if (shader) METHOD(shader,2,release_fn)(shader);
    if (surface) METHOD(surface,2,release_fn)(surface);
    if (back) METHOD(back,2,release_fn)(back);
    if (device) METHOD(device,2,release_fn)(device);
    if (window && destroy) destroy(window);
    if (d3d) METHOD(d3d,2,release_fn)(d3d);
#ifdef D3D9_PRESSURE_WINDOWS
    for (UINT i=0;i<D3D9_PRESSURE_WINDOWS;++i)
        if (pressure_windows[i] && destroy) destroy(pressure_windows[i]);
#endif
#if defined(D3D9_BENCHMARK) && defined(D3D9_CSMT_SETTING)
    if (config_changed) {
        int status = previous_status == 2 ? reg_delete(config,"csmt") :
            reg_set(config,"csmt",0,previous_type,previous,previous_size);
        if (status) { failed=1; REPORT("D3D9BENCH FAIL restore-setting\n"); }
        else REPORT("D3D9BENCH PASS restore-setting\n");
    }
    if (config) reg_close(config);
#endif
    VALUE("D3D9PROV DONE failures=",failed);
    ExitProcess(failed);
}
