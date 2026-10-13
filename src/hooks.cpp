#include "hooks.h"
#include "log.h"

#include <MinHook.h>
#include <cstring>

void* HookIAT(const char* dll, const char* func, void* replacement)
{
    auto base = (BYTE*)GetModuleHandleW(nullptr);
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;

    HMODULE mod = GetModuleHandleA(dll);
    void* resolved = mod ? (void*)GetProcAddress(mod, func) : nullptr;

    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
        auto iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        auto names = imp->OriginalFirstThunk ? (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk) : nullptr;
        for (int i = 0; iat[i].u1.Function; ++i) {
            bool match = false;
            if (names && !IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) {
                auto byName = (IMAGE_IMPORT_BY_NAME*)(base + names[i].u1.AddressOfData);
                match = strcmp((const char*)byName->Name, func) == 0;
            } else if (resolved) {
                match = (void*)iat[i].u1.Function == resolved;
            }
            if (!match) continue;
            void* old = (void*)iat[i].u1.Function;
            DWORD prot;
            VirtualProtect(&iat[i].u1.Function, sizeof(void*), PAGE_READWRITE, &prot);
            iat[i].u1.Function = (ULONG_PTR)replacement;
            VirtualProtect(&iat[i].u1.Function, sizeof(void*), prot, &prot);
            Log("IAT hook %s!%s ok", dll, func);
            return old;
        }
    }
    Log("IAT hook %s!%s NOT FOUND", dll, func);
    return nullptr;
}

bool HookFunction(void* target, void* detour, void** original)
{
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) {
        Log("MH_CreateHook(%p) failed: %s", target, MH_StatusToString(s));
        return false;
    }
    s = MH_EnableHook(target);
    if (s != MH_OK && s != MH_ERROR_ENABLED) {
        Log("MH_EnableHook(%p) failed: %s", target, MH_StatusToString(s));
        return false;
    }
    return true;
}

void HooksInit()
{
    MH_STATUS s = MH_Initialize();
    Log("MH_Initialize: %s", MH_StatusToString(s));
}
