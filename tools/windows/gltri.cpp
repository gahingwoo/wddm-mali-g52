// SPDX-License-Identifier: GPL-2.0
//
// gltri: an OpenGL context in a window, one triangle, and the pixel under
// it read back. The same source is built for ARM64, x64 and x86.
//
//   gltri.exe [system | <path to an opengl32.dll>] [--expect <text>] [--frames <n>]
//
// system (the default) is what a real program does: Windows' own
// opengl32.dll, which loads the ICD the display adapter names
// (OpenGLDriverName in the adapter's key, or the Wow value for an x86
// process). A path instead loads that opengl32.dll, Mesa's stand-in for
// Windows' (src/gallium/targets/libgl-gdi), which calls the ICD's Drv*
// entry points in the libgallium_wgl.dll beside it: the ICD's code with no
// adapter key needed, which is how CI runs it.
//
// It prints, in order:
//   - what the process is (built for, IsWow64Process2);
//   - for the adapter the window is on, what the kernel hands this process
//     as its OpenGL ICD (D3DKMTQueryAdapterInfo, KMTQAITYPE_UMOPENGLINFO):
//     the name, version and flags the INF registered, or a status;
//   - the context it got: a 3.3 core context through
//     WGL_ARB_create_context if that works, else 3.0, else the legacy
//     wglCreateContext one; then GL_VENDOR, GL_RENDERER, GL_VERSION,
//     GL_SHADING_LANGUAGE_VERSION and whether some extensions GL 2.x and
//     3.x programs ask for are there;
//   - one red triangle on blue, a vertex and a fragment shader, the centre
//     and a corner pixel read back with glReadPixels before SwapBuffers;
//   - frames (each a clear, the draw and SwapBuffers) a second over
//     --frames presents, 10 by default;
//   - every loaded module whose name has opengl32, gallium or mali in it,
//     with the machine type in its PE header.
//
// --expect <text>: GL_RENDERER must contain it (case does not matter).
//
// Exit code: 0 the pixels are right; 1 a pixel is wrong (expected where
// nothing executes the GPU's jobs, as with MALIKM_FAKE); 2 no pixel format
// or no context; 3 the shaders did not compile or link; 4 GL_RENDERER is
// not what --expect says; 5 the GL version is below 2.0; 100 any other
// failure.
//
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "psapi.lib")

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned char GLubyte;
typedef unsigned int GLbitfield;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef unsigned char GLboolean;

#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_NUM_EXTENSIONS 0x821D
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_TRIANGLES 0x0004
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_BACK 0x0405
#define GL_NO_ERROR 0

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

#define GLAPI_ APIENTRY
typedef void(GLAPI_ *PFN_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void(GLAPI_ *PFN_glClear)(GLbitfield);
typedef void(GLAPI_ *PFN_glViewport)(GLint, GLint, GLsizei, GLsizei);
typedef void(GLAPI_ *PFN_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
typedef void(GLAPI_ *PFN_glReadBuffer)(GLenum);
typedef const GLubyte *(GLAPI_ *PFN_glGetString)(GLenum);
typedef const GLubyte *(GLAPI_ *PFN_glGetStringi)(GLenum, GLuint);
typedef void(GLAPI_ *PFN_glGetIntegerv)(GLenum, GLint *);
typedef GLenum(GLAPI_ *PFN_glGetError)(void);
typedef void(GLAPI_ *PFN_glFinish)(void);
typedef void(GLAPI_ *PFN_glDrawArrays)(GLenum, GLint, GLsizei);
typedef GLuint(GLAPI_ *PFN_glCreateShader)(GLenum);
typedef void(GLAPI_ *PFN_glShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void(GLAPI_ *PFN_glCompileShader)(GLuint);
typedef void(GLAPI_ *PFN_glGetShaderiv)(GLuint, GLenum, GLint *);
typedef void(GLAPI_ *PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef GLuint(GLAPI_ *PFN_glCreateProgram)(void);
typedef void(GLAPI_ *PFN_glAttachShader)(GLuint, GLuint);
typedef void(GLAPI_ *PFN_glBindAttribLocation)(GLuint, GLuint, const GLchar *);
typedef void(GLAPI_ *PFN_glLinkProgram)(GLuint);
typedef void(GLAPI_ *PFN_glGetProgramiv)(GLuint, GLenum, GLint *);
typedef void(GLAPI_ *PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void(GLAPI_ *PFN_glUseProgram)(GLuint);
typedef void(GLAPI_ *PFN_glGenBuffers)(GLsizei, GLuint *);
typedef void(GLAPI_ *PFN_glBindBuffer)(GLenum, GLuint);
typedef void(GLAPI_ *PFN_glBufferData)(GLenum, GLsizeiptr, const void *, GLenum);
typedef void(GLAPI_ *PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void(GLAPI_ *PFN_glEnableVertexAttribArray)(GLuint);
typedef void(GLAPI_ *PFN_glGenVertexArrays)(GLsizei, GLuint *);
typedef void(GLAPI_ *PFN_glBindVertexArray)(GLuint);

typedef HGLRC(WINAPI *PFN_wglCreateContext)(HDC);
typedef BOOL(WINAPI *PFN_wglDeleteContext)(HGLRC);
typedef BOOL(WINAPI *PFN_wglMakeCurrent)(HDC, HGLRC);
typedef PROC(WINAPI *PFN_wglGetProcAddress)(LPCSTR);
typedef int(WINAPI *PFN_wglChoosePixelFormat)(HDC, const PIXELFORMATDESCRIPTOR *);
typedef BOOL(WINAPI *PFN_wglSetPixelFormat)(HDC, int, const PIXELFORMATDESCRIPTOR *);
typedef BOOL(WINAPI *PFN_wglSwapBuffers)(HDC);
typedef int(WINAPI *PFN_wglDescribePixelFormat)(HDC, int, UINT, PIXELFORMATDESCRIPTOR *);
typedef HGLRC(WINAPI *PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int *);
typedef const char *(WINAPI *PFN_wglGetExtensionsStringARB)(HDC);

static HMODULE gl;
static PFN_wglGetProcAddress p_wglGetProcAddress;

// A GL entry point: GL 1.1 ones are exports of opengl32.dll; the rest come
// from wglGetProcAddress, which may answer 1, 2, 3 or -1 for "no".
static void *glproc(const char *name)
{
    void *p = p_wglGetProcAddress ? (void *)p_wglGetProcAddress(name) : NULL;
    if (p == (void *)1 || p == (void *)2 || p == (void *)3 || p == (void *)-1)
        p = NULL;
    if (!p)
        p = (void *)GetProcAddress(gl, name);
    return p;
}

static const char *built_for(void)
{
#if defined(_M_ARM64EC)
    return "ARM64EC";
#elif defined(_M_ARM64)
    return "ARM64";
#elif defined(_M_X64)
    return "x64";
#elif defined(_M_IX86)
    return "x86";
#else
    return "?";
#endif
}

static const char *machine_name(USHORT m)
{
    switch (m) {
    case IMAGE_FILE_MACHINE_UNKNOWN: return "none";
    case IMAGE_FILE_MACHINE_I386:    return "x86";
    case IMAGE_FILE_MACHINE_AMD64:   return "x64";
    case IMAGE_FILE_MACHINE_ARM64:   return "ARM64";
    case 0xA641:                     return "ARM64EC";
    case 0xA64E:                     return "ARM64X";
    case IMAGE_FILE_MACHINE_ARMNT:   return "ARM32";
    default:                         return "other";
    }
}

static void print_process(void)
{
    typedef BOOL(WINAPI * WOW2)(HANDLE, USHORT *, USHORT *);
    WOW2 w2 = (WOW2)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2");
    USHORT pm = 0, nm = 0;
    printf("process: built for %s", built_for());
    if (w2 && w2(GetCurrentProcess(), &pm, &nm))
        printf(", IsWow64Process2 process 0x%04x (%s) native 0x%04x (%s)", pm,
               pm ? machine_name(pm) : "not WOW64", nm, machine_name(nm));
    printf(", pointer %u bytes\n", (unsigned)sizeof(void *));
}

// The ICD the kernel names for this process on the adapter that shows the
// window: what Windows' opengl32.dll will load. A WOW64 process is given
// the Wow values.
static void print_gl_icd(HDC dc)
{
    D3DKMT_OPENADAPTERFROMHDC o = {};
    o.hDc = dc;
    NTSTATUS st = D3DKMTOpenAdapterFromHdc(&o);
    if (st < 0) {
        printf("adapter of the window: D3DKMTOpenAdapterFromHdc 0x%08lx\n", (unsigned long)st);
        return;
    }
    D3DKMT_OPENGLINFO info = {};
    D3DKMT_QUERYADAPTERINFO q = {};
    q.hAdapter = o.hAdapter;
    q.Type = KMTQAITYPE_UMOPENGLINFO;
    q.pPrivateDriverData = &info;
    q.PrivateDriverDataSize = sizeof(info);
    st = D3DKMTQueryAdapterInfo(&q);
    printf("adapter of the window: luid %08lx:%08lx, source %u\n", o.AdapterLuid.HighPart,
           o.AdapterLuid.LowPart, (unsigned)o.VidPnSourceId);
    if (st < 0)
        printf("  OpenGL ICD: status 0x%08lx (none registered)\n", (unsigned long)st);
    else
        printf("  OpenGL ICD: \"%ls\", version %lu, flags 0x%lx\n", info.UmdOpenGlIcdFileName,
               (unsigned long)info.Version, (unsigned long)info.Flags);
    D3DKMT_CLOSEADAPTER c = {};
    c.hAdapter = o.hAdapter;
    D3DKMTCloseAdapter(&c);
}

static void print_modules(void)
{
    HMODULE mods[1024];
    DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        printf("modules: EnumProcessModules failed (%lu)\n", GetLastError());
        return;
    }
    unsigned n = need / sizeof(HMODULE);
    if (n > 1024)
        n = 1024;
    for (unsigned i = 0; i < n; i++) {
        wchar_t path[MAX_PATH * 2], lower[MAX_PATH * 2];
        if (!GetModuleFileNameExW(GetCurrentProcess(), mods[i], path, MAX_PATH * 2))
            continue;
        wcsncpy_s(lower, path, _TRUNCATE);
        _wcslwr_s(lower);
        const wchar_t *base = wcsrchr(lower, L'\\');
        base = base ? base + 1 : lower;
        if (!wcsstr(base, L"opengl32") && !wcsstr(base, L"gallium") && !wcsstr(base, L"mali"))
            continue;
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)mods[i];
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const BYTE *)mods[i] + dos->e_lfanew);
        printf("module: %ls (header machine %s)\n", path, machine_name(nt->FileHeader.Machine));
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

static void pump(void)
{
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
}

static bool contains_nocase(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; hay && *hay; hay++)
        if (_strnicmp(hay, needle, n) == 0)
            return true;
    return false;
}

#define W 128
#define H 128

int main(int argc, char **argv)
{
    const char *which = "system", *expect = NULL;
    int frames = 10;

    setvbuf(stdout, NULL, _IONBF, 0);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--expect") == 0 && i + 1 < argc)
            expect = argv[++i];
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            frames = atoi(argv[++i]);
        else
            which = argv[i];
    }
    bool use_system = _stricmp(which, "system") == 0;
    print_process();

    // Windows' opengl32.dll from System32 (SysWOW64 for x86, by the file
    // system redirector), or the one named, with its own directory searched
    // for what it imports (libgallium_wgl.dll).
    if (use_system) {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        wcscpy_s(path + n, MAX_PATH - n, L"\\opengl32.dll");
        gl = LoadLibraryW(path);
    } else {
        gl = LoadLibraryExA(which, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    if (!gl) {
        printf("FAIL loading %s: error %lu\n", which, GetLastError());
        return 100;
    }
    wchar_t glpath[MAX_PATH * 2];
    GetModuleFileNameW(gl, glpath, MAX_PATH * 2);
    printf("opengl32: %ls (%s)\n", glpath, use_system ? "Windows', loads the adapter's ICD" : "Mesa's, calls libgallium_wgl.dll");

#define WGL(n) PFN_##n p_##n = (PFN_##n)(void *)GetProcAddress(gl, #n)
    WGL(wglCreateContext);
    WGL(wglDeleteContext);
    WGL(wglMakeCurrent);
    WGL(wglChoosePixelFormat);
    WGL(wglSetPixelFormat);
    WGL(wglSwapBuffers);
    WGL(wglDescribePixelFormat);
#undef WGL
    p_wglGetProcAddress = (PFN_wglGetProcAddress)(void *)GetProcAddress(gl, "wglGetProcAddress");
    if (!p_wglCreateContext || !p_wglMakeCurrent || !p_wglGetProcAddress ||
        (!use_system && (!p_wglChoosePixelFormat || !p_wglSetPixelFormat || !p_wglSwapBuffers))) {
        printf("FAIL %ls lacks a wgl entry point this test calls\n", glpath);
        return 100;
    }

    WNDCLASSA wc = {};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "mali-gltri";
    RegisterClassA(&wc);
    RECT r = {0, 0, W, H};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("mali-gltri", "mali-gltri", WS_OVERLAPPEDWINDOW, 40, 40,
                              r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        printf("FAIL CreateWindow: %lu\n", GetLastError());
        return 100;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    pump();
    HDC dc = GetDC(hwnd);
    print_gl_icd(dc);

    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    // Windows' opengl32 is reached through gdi32 for these, as a program
    // does; Mesa's stand-in is called directly.
    int pf = use_system ? ChoosePixelFormat(dc, &pfd) : p_wglChoosePixelFormat(dc, &pfd);
    BOOL set = pf && (use_system ? SetPixelFormat(dc, pf, &pfd) : p_wglSetPixelFormat(dc, pf, &pfd));
    if (!pf || !set) {
        printf("FAIL pixel format %d, set %d, error %lu\n", pf, set, GetLastError());
        return 2;
    }
    if (use_system)
        DescribePixelFormat(dc, pf, sizeof(pfd), &pfd);
    else if (p_wglDescribePixelFormat)
        p_wglDescribePixelFormat(dc, pf, sizeof(pfd), &pfd);
    printf("pixel format %d: color %u alpha %u depth %u stencil %u, flags 0x%lx%s\n", pf,
           pfd.cColorBits, pfd.cAlphaBits, pfd.cDepthBits, pfd.cStencilBits,
           (unsigned long)pfd.dwFlags,
           (pfd.dwFlags & PFD_GENERIC_FORMAT) ? " (GENERIC: Windows' software GL)" : "");

    HGLRC legacy = p_wglCreateContext(dc);
    if (!legacy || !p_wglMakeCurrent(dc, legacy)) {
        printf("FAIL wglCreateContext %p: error %lu\n", (void *)legacy, GetLastError());
        return 2;
    }
    HGLRC rc = legacy;
    bool core = false;
    const char *how = "wglCreateContext";
    PFN_wglCreateContextAttribsARB cca = (PFN_wglCreateContextAttribsARB)glproc("wglCreateContextAttribsARB");
    PFN_wglGetExtensionsStringARB wext = (PFN_wglGetExtensionsStringARB)glproc("wglGetExtensionsStringARB");
    printf("WGL extensions: %s\n", wext ? wext(dc) : "(no wglGetExtensionsStringARB)");
    if (cca) {
        static const struct { int major, minor; bool core; const char *name; } tries[] = {
            {3, 3, true, "3.3 core"}, {3, 0, false, "3.0"},
        };
        for (const auto &t : tries) {
            int attrs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, t.major, WGL_CONTEXT_MINOR_VERSION_ARB, t.minor,
                           t.core ? WGL_CONTEXT_PROFILE_MASK_ARB : 0, t.core ? WGL_CONTEXT_CORE_PROFILE_BIT_ARB : 0,
                           0};
            HGLRC n = cca(dc, NULL, attrs);
            printf("wglCreateContextAttribsARB %s: %s\n", t.name, n ? "yes" : "no");
            if (n && p_wglMakeCurrent(dc, n)) {
                rc = n;
                core = t.core;
                how = t.name;
                p_wglDeleteContext(legacy);
                break;
            }
            if (n)
                p_wglDeleteContext(n);
            p_wglMakeCurrent(dc, legacy);
        }
    }
    printf("context: %s\n", how);

#define GLF(n) PFN_##n p_##n = (PFN_##n)glproc(#n)
    GLF(glClearColor); GLF(glClear); GLF(glViewport); GLF(glReadPixels); GLF(glReadBuffer);
    GLF(glGetString); GLF(glGetStringi); GLF(glGetIntegerv); GLF(glGetError); GLF(glFinish);
    GLF(glDrawArrays); GLF(glCreateShader); GLF(glShaderSource); GLF(glCompileShader);
    GLF(glGetShaderiv); GLF(glGetShaderInfoLog); GLF(glCreateProgram); GLF(glAttachShader);
    GLF(glBindAttribLocation); GLF(glLinkProgram); GLF(glGetProgramiv); GLF(glGetProgramInfoLog);
    GLF(glUseProgram); GLF(glGenBuffers); GLF(glBindBuffer); GLF(glBufferData);
    GLF(glVertexAttribPointer); GLF(glEnableVertexAttribArray); GLF(glGenVertexArrays);
    GLF(glBindVertexArray);
#undef GLF
    if (!p_glGetString) {
        printf("FAIL no glGetString\n");
        return 100;
    }
    const char *vendor = (const char *)p_glGetString(GL_VENDOR);
    const char *renderer = (const char *)p_glGetString(GL_RENDERER);
    const char *version = (const char *)p_glGetString(GL_VERSION);
    const char *glsl = (const char *)p_glGetString(GL_SHADING_LANGUAGE_VERSION);
    printf("GL_VENDOR: %s\nGL_RENDERER: %s\nGL_VERSION: %s\nGL_SHADING_LANGUAGE_VERSION: %s\n",
           vendor ? vendor : "(null)", renderer ? renderer : "(null)", version ? version : "(null)",
           glsl ? glsl : "(null)");
    int major = version ? atoi(version) : 0;

    // Extensions that GL 2.x and 3.x programs (GpuTest's among them) look
    // for; from glGetStringi in a core context, GL_EXTENSIONS otherwise.
    static const char *const wanted[] = {
        "GL_ARB_vertex_buffer_object", "GL_ARB_framebuffer_object", "GL_EXT_framebuffer_object",
        "GL_ARB_texture_float", "GL_ARB_vertex_array_object", "GL_ARB_uniform_buffer_object",
        "GL_ARB_texture_non_power_of_two", "GL_ARB_geometry_shader4", "GL_EXT_texture_filter_anisotropic",
        "GL_ARB_timer_query",
    };
    GLint next = 0;
    if (core && p_glGetStringi && p_glGetIntegerv)
        p_glGetIntegerv(GL_NUM_EXTENSIONS, &next);
    const char *exts = core ? NULL : (const char *)p_glGetString(GL_EXTENSIONS);
    for (const char *w : wanted) {
        bool have = false;
        if (exts) {
            size_t n = strlen(w);
            for (const char *e = strstr(exts, w); e && !have; e = strstr(e + 1, w))
                have = e[n] == ' ' || e[n] == 0;
        } else {
            for (GLint i = 0; i < next && !have; i++)
                have = strcmp((const char *)p_glGetStringi(GL_EXTENSIONS, (GLuint)i), w) == 0;
        }
        printf("  %-36s %s\n", w, have ? "yes" : "no");
    }

    if (expect && !contains_nocase(renderer, expect)) {
        printf("FAIL GL_RENDERER is not %s\n", expect);
        return 4;
    }
    if (major < 2 || !p_glCreateShader || !p_glGenBuffers || !p_glVertexAttribPointer) {
        printf("FAIL GL %d: no shaders or no vertex buffers\n", major);
        return 5;
    }

    const char *vs_src = core
        ? "#version 330 core\nlayout(location = 0) in vec2 pos;\nvoid main() { gl_Position = vec4(pos, 0.0, 1.0); }\n"
        : "#version 110\nattribute vec2 pos;\nvoid main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
    const char *fs_src = core
        ? "#version 330 core\nout vec4 color;\nvoid main() { color = vec4(1.0, 0.0, 0.0, 1.0); }\n"
        : "#version 110\nvoid main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }\n";
    GLuint sh[2];
    const char *src[2] = {vs_src, fs_src};
    for (int i = 0; i < 2; i++) {
        GLint ok = 0;
        char log[1024] = "";
        sh[i] = p_glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
        p_glShaderSource(sh[i], 1, &src[i], NULL);
        p_glCompileShader(sh[i]);
        p_glGetShaderiv(sh[i], GL_COMPILE_STATUS, &ok);
        if (!ok) {
            p_glGetShaderInfoLog(sh[i], sizeof log, NULL, log);
            printf("FAIL %s shader: %s\n", i ? "fragment" : "vertex", log);
            return 3;
        }
    }
    GLuint prog = p_glCreateProgram();
    p_glAttachShader(prog, sh[0]);
    p_glAttachShader(prog, sh[1]);
    p_glBindAttribLocation(prog, 0, "pos");
    p_glLinkProgram(prog);
    GLint linked = 0;
    p_glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024] = "";
        p_glGetProgramInfoLog(prog, sizeof log, NULL, log);
        printf("FAIL link: %s\n", log);
        return 3;
    }
    printf("shaders: compiled and linked\n");

    static const GLfloat tri[] = {-0.8f, -0.8f, 0.8f, -0.8f, 0.0f, 0.8f};
    GLuint vao = 0, vbo = 0;
    if (core) {
        p_glGenVertexArrays(1, &vao);
        p_glBindVertexArray(vao);
    }
    p_glGenBuffers(1, &vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, NULL);
    p_glEnableVertexAttribArray(0);
    p_glUseProgram(prog);
    p_glViewport(0, 0, W, H);

    unsigned char centre[4] = {0}, corner[4] = {0};
    LARGE_INTEGER hz, t0, t1;
    QueryPerformanceFrequency(&hz);
    for (int f = 0; f <= frames; f++) {
        if (f == 1)
            QueryPerformanceCounter(&t0);   // frame 0 includes the shader compiles
        p_glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
        p_glClear(GL_COLOR_BUFFER_BIT);
        p_glDrawArrays(GL_TRIANGLES, 0, 3);
        if (f == 0) {
            p_glReadBuffer(GL_BACK);
            p_glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, centre);
            p_glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
        }
        BOOL swapped = use_system ? SwapBuffers(dc) : p_wglSwapBuffers(dc);
        if (!swapped) {
            printf("FAIL SwapBuffers, frame %d: error %lu\n", f, GetLastError());
            return 100;
        }
        pump();
    }
    QueryPerformanceCounter(&t1);
    double secs = frames > 0 ? (double)(t1.QuadPart - t0.QuadPart) / (double)hz.QuadPart : 0.0;
    GLenum err = p_glGetError();
    printf("presents: %d in %.3f s, %.1f a second, %ux%u; glGetError 0x%x\n", frames, secs,
           secs > 0 ? frames / secs : 0.0, W, H, err);
    print_modules();

    bool red = centre[0] > 200 && centre[1] < 50 && centre[2] < 50;
    bool blue = corner[0] < 50 && corner[1] < 50 && corner[2] > 200;
    printf("centre %u,%u,%u,%u (%s), corner %u,%u,%u,%u (%s)\n", centre[0], centre[1], centre[2],
           centre[3], red ? "red" : "not red", corner[0], corner[1], corner[2], corner[3],
           blue ? "blue" : "not blue");

    p_wglMakeCurrent(NULL, NULL);
    p_wglDeleteContext(rc);
    ReleaseDC(hwnd, dc);
    DestroyWindow(hwnd);
    printf("%s\n", red && blue ? "PASS" : "FAIL pixels");
    return red && blue ? 0 : 1;
}
