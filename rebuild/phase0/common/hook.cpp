#include "hook.h"
#include <cstring>

void* HookImport(HMODULE module, const char* dll, const char* func, void* replacement) {
    if (!module) return nullptr;
    auto base = (BYTE*)module;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;

    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
        auto names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto addrs = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addrs) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto byName = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)byName->Name, func) != 0) continue;
            void* previous = (void*)addrs->u1.Function;
            DWORD old;
            VirtualProtect(&addrs->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            addrs->u1.Function = (ULONG_PTR)replacement;
            VirtualProtect(&addrs->u1.Function, sizeof(void*), old, &old);
            return previous;
        }
    }
    return nullptr;
}

bool PatchBytes(void* address, const unsigned char* expected, const char* mask,
                const unsigned char* replacement, size_t length) {
    auto p = (unsigned char*)address;
    for (size_t i = 0; i < length; ++i)
        if (mask[i] != '?' && p[i] != expected[i]) return false;
    DWORD old;
    if (!VirtualProtect(p, length, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(p, replacement, length);
    VirtualProtect(p, length, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, length);
    return true;
}

void* InstallDetour(void* target, void* detour, size_t stolenBytes) {
    auto src = (BYTE*)target;
    auto tramp = (BYTE*)VirtualAlloc(nullptr, stolenBytes + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return nullptr;
    memcpy(tramp, src, stolenBytes);
    tramp[stolenBytes] = 0xE9;  // jmp back past the stolen instructions
    *(INT32*)(tramp + stolenBytes + 1) = (INT32)((src + stolenBytes) - (tramp + stolenBytes + 5));

    DWORD old;
    VirtualProtect(src, stolenBytes, PAGE_EXECUTE_READWRITE, &old);
    src[0] = 0xE9;
    *(INT32*)(src + 1) = (INT32)((BYTE*)detour - (src + 5));
    for (size_t i = 5; i < stolenBytes; ++i) src[i] = 0x90;
    VirtualProtect(src, stolenBytes, old, &old);
    FlushInstructionCache(GetCurrentProcess(), src, stolenBytes);
    return tramp;
}
