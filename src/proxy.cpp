// dsound.dll proxy: the game imports DSOUND.dll, so Windows loads this DLL from
// the game folder. Every export simply forwards to the real system dsound.dll.
#include <windows.h>
#include <mmsystem.h>

static HMODULE g_real = nullptr;

static FARPROC Real(const char* name)
{
    if (!g_real) {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\dsound.dll");
        g_real = LoadLibraryW(path);
        if (!g_real) return nullptr;
    }
    return GetProcAddress(g_real, name);
}

#define FORWARD(ret, name, params, args)                                  \
    extern "C" ret WINAPI Proxy_##name params                             \
    {                                                                     \
        using Fn = ret(WINAPI*) params;                                   \
        static Fn fn = (Fn)Real(#name);                                   \
        if (!fn) return (ret)E_FAIL;                                      \
        return fn args;                                                   \
    }

FORWARD(HRESULT, DirectSoundCreate, (LPCGUID a, void** b, void* c), (a, b, c))
FORWARD(HRESULT, DirectSoundEnumerateA, (void* a, void* b), (a, b))
FORWARD(HRESULT, DirectSoundEnumerateW, (void* a, void* b), (a, b))
FORWARD(HRESULT, DllCanUnloadNow, (), ())
FORWARD(HRESULT, DllGetClassObject, (REFCLSID a, REFIID b, void** c), (a, b, c))
FORWARD(HRESULT, DirectSoundCaptureCreate, (LPCGUID a, void** b, void* c), (a, b, c))
FORWARD(HRESULT, DirectSoundCaptureEnumerateA, (void* a, void* b), (a, b))
FORWARD(HRESULT, DirectSoundCaptureEnumerateW, (void* a, void* b), (a, b))
FORWARD(HRESULT, GetDeviceID, (LPCGUID a, LPGUID b), (a, b))
FORWARD(HRESULT, DirectSoundFullDuplexCreate,
        (LPCGUID a, LPCGUID b, void* c, void* d, HWND e, DWORD f, void** g, void** h, void** i, void* j),
        (a, b, c, d, e, f, g, h, i, j))
FORWARD(HRESULT, DirectSoundCreate8, (LPCGUID a, void** b, void* c), (a, b, c))
FORWARD(HRESULT, DirectSoundCaptureCreate8, (LPCGUID a, void** b, void* c), (a, b, c))
