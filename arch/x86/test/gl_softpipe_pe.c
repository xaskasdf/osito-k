/* OpenGL pixels and GLSL compilation, usable as a native Windows oracle. */
typedef unsigned int UINT,DWORD;
typedef int BOOL;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(UINT);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE,const char *);
int _fltused;
#define SAY(text) do {const char *s=(text);DWORD n=0,w;while(s[n])n++; \
    WriteFile(GetStdHandle((DWORD)-11),s,n,&w,0);} while(0)
#define CHECK(value,label) do {if(!(value)){SAY("GLSOFT FAIL " label "\n");ExitProcess(1);} \
    SAY("GLSOFT PASS " label "\n");} while(0)
#define LOAD(var,type,module,name) type var=(type)GetProcAddress(module,name);CHECK(var,name)
#define GLLOAD(var,type,name) type var=(type)proc(name);CHECK(var,name)
typedef HANDLE (CALL *window_fn)(DWORD,const char *,const char *,DWORD,int,int,int,int,HANDLE,HANDLE,HANDLE,void *);
typedef HANDLE (CALL *handle_fn)(HANDLE);
typedef BOOL (CALL *destroy_fn)(HANDLE);
typedef int (CALL *release_fn)(HANDLE,HANDLE);
typedef BOOL (CALL *current_fn)(HANDLE,HANDLE);
typedef const unsigned char *(CALL *string_fn)(UINT);
typedef void *(CALL *proc_fn)(const char *);
typedef void (CALL *clear_color_fn)(float,float,float,float);
typedef void (CALL *uint_fn)(UINT);
typedef void (CALL *void_fn)(void);
typedef void (CALL *read_fn)(int,int,int,int,UINT,UINT,void *);
typedef UINT (CALL *create_shader_fn)(UINT);
typedef void (CALL *source_fn)(UINT,int,const char *const *,const int *);
typedef void (CALL *get_shader_fn)(UINT,UINT,int *);
typedef UINT (CALL *create_program_fn)(void);
typedef void (CALL *attach_fn)(UINT,UINT);
typedef void (CALL *vertex_fn)(float,float);
typedef BOOL (CALL *set_format_fn)(HANDLE,int,const void *);
typedef void (CALL *programs_fn)(int,UINT *);
typedef void (CALL *program_string_fn)(UINT,UINT,int,const void *);
typedef unsigned char (CALL *is_shader_fn)(UINT);
typedef void (CALL *internal_format_fn)(UINT,UINT,UINT,int,int *);

void gl_softpipe_entry(void)
{
    HANDLE user=LoadLibraryA("user32.dll"),gdi=LoadLibraryA("gdi32.dll"),gl=LoadLibraryA("opengl32.dll");
    LOAD(window,window_fn,user,"CreateWindowExA");LOAD(dc,handle_fn,user,"GetDC");
    LOAD(destroy,destroy_fn,user,"DestroyWindow");LOAD(release,release_fn,user,"ReleaseDC");
    LOAD(create,handle_fn,gl,"wglCreateContext");LOAD(bind,current_fn,gl,"wglMakeCurrent");
    LOAD(del,destroy_fn,gl,"wglDeleteContext");LOAD(proc,proc_fn,gl,"wglGetProcAddress");
    LOAD(string,string_fn,gl,"glGetString");LOAD(clear_color,clear_color_fn,gl,"glClearColor");
    LOAD(clear,uint_fn,gl,"glClear");LOAD(read,read_fn,gl,"glReadPixels");
    LOAD(begin,uint_fn,gl,"glBegin");LOAD(vertex,vertex_fn,gl,"glVertex2f");
    LOAD(end,void_fn,gl,"glEnd");LOAD(swap,destroy_fn,gdi,"SwapBuffers");
    LOAD(set,set_format_fn,gdi,"SetPixelFormat");
    HANDLE wnd=window(0,"STATIC","GL softpipe probe",0x90000000,40,40,64,64,0,0,0,0);
    HANDLE hdc=wnd?dc(wnd):0;
    CHECK(hdc,"window DC");CHECK(set(hdc,1,0),"pixel format");
    HANDLE ctx=create(hdc);CHECK(ctx,"context");CHECK(bind(hdc,ctx),"bind");
    const unsigned char *renderer=string(0x1f01);
    CHECK(renderer,"renderer string");SAY("GLSOFT RENDERER ");SAY((const char *)renderer);SAY("\n");
    clear_color(1,0,0,1);clear(0x4000);
    unsigned char pixel[4]={0};read(32,32,1,1,0x1908,0x1401,pixel);
    CHECK(pixel[0]==255 && pixel[1]==0 && pixel[2]==0,"clear pixel");
    GLLOAD(internal,internal_format_fn,"glGetInternalformativ");
    int supported=0;internal(0x0de1,0x8058,0x826f,1,&supported);
    CHECK(supported,"RGBA8 internal format supported");
    GLLOAD(shader,create_shader_fn,"glCreateShader");GLLOAD(source,source_fn,"glShaderSource");
    GLLOAD(compile,uint_fn,"glCompileShader");GLLOAD(shader_status,get_shader_fn,"glGetShaderiv");
    GLLOAD(program,create_program_fn,"glCreateProgram");GLLOAD(attach,attach_fn,"glAttachShader");
    GLLOAD(link,uint_fn,"glLinkProgram");GLLOAD(program_status,get_shader_fn,"glGetProgramiv");
    GLLOAD(use,uint_fn,"glUseProgram");GLLOAD(delete_shader,uint_fn,"glDeleteShader");
    GLLOAD(delete_program,uint_fn,"glDeleteProgram");
    GLLOAD(is_shader,is_shader_fn,"glIsShader");
    GLLOAD(source_arb,source_fn,"glShaderSourceARB");
    const char *vs[]={"#version 120\n", "void main(){", "gl_Position=gl_Vertex;}\n"};
    const char *fs[]={"#version 120\n", "void main(){", "gl_FragColor=vec4(0.0,1.0,0.0,1.0);}\nIGNORED"};
    const int lengths[]={-1,-1,37};
    UINT vert=shader(0x8b31),frag=shader(0x8b30),prog=program();int status=0;
    CHECK(vert && frag && prog,"shader objects");
    source(vert,3,vs,0);compile(vert);shader_status(vert,0x8b81,&status);CHECK(status,"vertex compilation");
    source_arb(frag,3,fs,lengths);compile(frag);shader_status(frag,0x8b81,&status);CHECK(status,"fragment compilation");
    attach(prog,vert);attach(prog,frag);link(prog);program_status(prog,0x8b82,&status);CHECK(status,"link");
    use(prog);begin(4);vertex(-1,-1);vertex(1,-1);vertex(0,1);end();
    read(32,32,1,1,0x1908,0x1401,pixel);
    CHECK(pixel[0]==0 && pixel[1]==255 && pixel[2]==0,"shader triangle pixel");
    CHECK(swap(hdc),"present");use(0);
    LOAD(share,current_fn,gl,"wglShareLists");
    HANDLE shared=create(hdc);CHECK(shared,"second context");
    CHECK(share(ctx,shared),"share contexts");CHECK(bind(hdc,shared),"bind shared context");
    CHECK(is_shader(vert) && is_shader(frag),"shared shader names");
    use(prog);clear_color(0,0,0,1);clear(0x4000);
    begin(4);vertex(-1,-1);vertex(1,-1);vertex(0,1);end();
    read(32,32,1,1,0x1908,0x1401,pixel);
    CHECK(pixel[0]==0 && pixel[1]==255 && pixel[2]==0,"shared shader triangle pixel");
    use(0);CHECK(bind(hdc,ctx),"restore first context");
    CHECK(del(shared),"delete shared context");
    CHECK(is_shader(vert) && is_shader(frag),"shaders after shared context deletion");
    GLLOAD(gen_arb,programs_fn,"glGenProgramsARB");
    GLLOAD(bind_arb,attach_fn,"glBindProgramARB");
    GLLOAD(text_arb,program_string_fn,"glProgramStringARB");
    GLLOAD(delete_arb,programs_fn,"glDeleteProgramsARB");
    LOAD(enable,uint_fn,gl,"glEnable");LOAD(disable,uint_fn,gl,"glDisable");
    LOAD(error,create_program_fn,gl,"glGetError");
    CHECK(error()==0,"GLSL state before ARB");
    UINT arb[2]={0};gen_arb(2,arb);CHECK(arb[0] && arb[1],"ARB objects");
    static const char vp[]="!!ARBvp1.0\nMOV result.position, vertex.position;\nEND\n";
    static const char fp[]="!!ARBfp1.0\nMOV result.color, {0.0, 0.0, 1.0, 1.0};\nEND\n";
    bind_arb(0x8620,arb[0]);text_arb(0x8620,0x8875,3,"bad");
    CHECK(error()==0x502,"ARB rejects invalid program");
    text_arb(0x8620,0x8875,sizeof(vp)-1,vp);CHECK(error()==0,"ARB vertex compilation");
    bind_arb(0x8804,arb[1]);text_arb(0x8804,0x8875,sizeof(fp)-1,fp);
    CHECK(error()==0,"ARB fragment compilation");enable(0x8620);enable(0x8804);
    begin(4);vertex(-1,-1);vertex(1,-1);vertex(0,1);end();
    read(32,32,1,1,0x1908,0x1401,pixel);
    CHECK(error()==0 && pixel[0]==0 && pixel[1]==0 && pixel[2]==255,"ARB triangle pixel");
    disable(0x8620);disable(0x8804);delete_arb(2,arb);
    CHECK(is_shader(vert) && is_shader(frag),"shaders before program deletion");
    delete_program(prog);CHECK(error()==0,"delete GLSL program");
    CHECK(is_shader(vert) && is_shader(frag),"shaders after program deletion");
    delete_shader(vert);CHECK(error()==0,"delete vertex shader");
    delete_shader(frag);CHECK(error()==0,"delete fragment shader");
    CHECK(bind(0,0),"unbind");CHECK(del(ctx),"delete context");
    release(wnd,hdc);destroy(wnd);SAY("GLSOFT DONE\n");ExitProcess(0);
}
