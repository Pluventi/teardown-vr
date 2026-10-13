#pragma once
#include <windows.h>

// Patch an entry of the main executable's import table. Returns the previous
// function pointer (or nullptr if the import was not found).
void* HookIAT(const char* dll, const char* func, void* replacement);

// Inline hook through MinHook. Returns true on success.
bool HookFunction(void* target, void* detour, void** original);

void HooksInit();      // MinHook init + IAT hooks; safe to call from DllMain
