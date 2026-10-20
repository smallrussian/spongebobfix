// Fixes for running SpongeBob: Employee of the Month on modern Windows, installed into the
// already-loaded sb2.exe and OMT.dll when this DLL is loaded (before either runs any code).
#include "ddraw_impl.h"
#include "config.h"
#include "hook.h"
#include <mmsystem.h>

#undef LOG_MODULE
#define LOG_MODULE "compat"

namespace {

// --- Precise millisecond clock (the engine only uses GetTickCount, ~15.6 ms resolution) ------
LARGE_INTEGER g_freq, g_start;
DWORD g_tickBase;

DWORD WINAPI PreciseGetTickCount() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return g_tickBase + (DWORD)((now.QuadPart - g_start.QuadPart) * 1000 / g_freq.QuadPart);
}

// --- Main-loop idling ---------------------------------------------------------------------------
// OMediaPeriodical::devote_time() is the engine's whole main loop body; OMediaApplication::start
// calls it in a tight loop forever. When an iteration produced no frame, wait a millisecond
// instead of spinning a core at 100%.
using DevoteFn = void(__cdecl*)();
DevoteFn g_origDevote;
HANDLE g_idleTimer;

void IdleWait() {
    if (g_idleTimer) {
        LARGE_INTEGER due;
        due.QuadPart = -10000;  // 1 ms
        SetWaitableTimer(g_idleTimer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(g_idleTimer, 5);
    } else {
        Sleep(1);
    }
}

void __cdecl DevoteTimeHook() {
    uint64_t before = Renderer::PresentCount();
    g_origDevote();
    Renderer::PresentIfDirty();
    if (Renderer::PresentCount() == before) IdleWait();
}

// --- Window and mouse ---------------------------------------------------------------------------
decltype(&CreateWindowExA) g_createWindow;
decltype(&GetCursorPos) g_getCursorPos;
decltype(&SetCursorPos) g_setCursorPos;
decltype(&GetClientRect) g_getClientRect;
decltype(&SetWindowPos) g_setWindowPos;

HWND WINAPI HookCreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h,
                                HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    // The engine makes its display window always-on-top, which breaks Alt+Tab on modern desktops.
    return g_createWindow(ex & ~WS_EX_TOPMOST, cls, name, style, x, y, w, h, parent, menu, inst, param);
}

BOOL WINAPI HookGetCursorPos(LPPOINT p) {
    if (!g_getCursorPos(p)) return FALSE;
    HWND w = Renderer::Window();
    if (w) {
        // The game assumes a 640x480 screen at the origin; report positions in that space.
        ScreenToClient(w, p);
        *p = Renderer::ClientToGame(*p);
    }
    return TRUE;
}

decltype(&GetAsyncKeyState) g_getAsyncKeyState;

// The engine polls mouse buttons and keys globally. Now that the game is a window among others,
// only report input while it is the foreground window, and mouse buttons only over the image.
SHORT WINAPI HookGetAsyncKeyState(int vk) {
    HWND w = Renderer::Window();
    if (w) {
        if (GetForegroundWindow() != w) return 0;
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) {
            POINT p;
            g_getCursorPos(&p);
            ScreenToClient(w, &p);
            if (!Renderer::IsInsideGameImage(p)) return 0;
        }
    }
    return g_getAsyncKeyState(vk);
}

BOOL WINAPI HookSetCursorPos(int x, int y) {
    HWND w = Renderer::Window();
    if (!w) return g_setCursorPos(x, y);
    POINT p = Renderer::GameToClient({x, y});
    ClientToScreen(w, &p);
    return g_setCursorPos(p.x, p.y);
}

BOOL WINAPI HookGetClientRect(HWND hwnd, LPRECT r) {
    if (hwnd && hwnd == Renderer::Window() && r) {
        *r = {0, 0, g_mode.width, g_mode.height};
        return TRUE;
    }
    return g_getClientRect(hwnd, r);
}

BOOL WINAPI HookSetWindowPos(HWND hwnd, HWND after, int x, int y, int w, int h, UINT flags) {
    if (hwnd && hwnd == Renderer::Window()) return TRUE;  // window layout is ours now
    return g_setWindowPos(hwnd, after, x, y, w, h, flags);
}

void EnableDpiAwareness() {
    using Fn = BOOL(WINAPI*)(HANDLE);
    if (auto f = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))
        f((HANDLE)-4);  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
}

}  // namespace

namespace Compat {

void Install() {
    const Config& cfg = GetConfig();
    HMODULE exe = GetModuleHandleW(nullptr);
    HMODULE omt = GetModuleHandleW(L"OMT.dll");
    LOG_INFO("installing compatibility fixes (exe=%p, OMT=%p)", exe, omt);

    EnableDpiAwareness();
    timeBeginPeriod(1);

    if (cfg.preciseTimer) {
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_start);
        g_tickBase = GetTickCount();
        int n = 0;
        for (HMODULE m : {exe, omt})
            if (HookImport(m, "KERNEL32.dll", "GetTickCount", (void*)PreciseGetTickCount)) ++n;
        LOG_INFO("precise timer: %d module(s) hooked", n);
    }

    if (omt) {
        g_createWindow = (decltype(g_createWindow))HookImport(omt, "USER32.dll", "CreateWindowExA", (void*)HookCreateWindowExA);
        g_getCursorPos = (decltype(g_getCursorPos))HookImport(omt, "USER32.dll", "GetCursorPos", (void*)HookGetCursorPos);
        g_setCursorPos = (decltype(g_setCursorPos))HookImport(omt, "USER32.dll", "SetCursorPos", (void*)HookSetCursorPos);
        g_getClientRect = (decltype(g_getClientRect))HookImport(omt, "USER32.dll", "GetClientRect", (void*)HookGetClientRect);
        g_setWindowPos = (decltype(g_setWindowPos))HookImport(omt, "USER32.dll", "SetWindowPos", (void*)HookSetWindowPos);
        g_getAsyncKeyState = (decltype(g_getAsyncKeyState))HookImport(omt, "USER32.dll", "GetAsyncKeyState", (void*)HookGetAsyncKeyState);
        if (!g_getAsyncKeyState) g_getAsyncKeyState = GetAsyncKeyState;
        // Restore any hook that failed so the engine still reaches the real function.
        if (!g_createWindow) g_createWindow = CreateWindowExA;
        if (!g_getCursorPos) g_getCursorPos = GetCursorPos;
        if (!g_setCursorPos) g_setCursorPos = SetCursorPos;
        if (!g_getClientRect) g_getClientRect = GetClientRect;
        if (!g_setWindowPos) g_setWindowPos = SetWindowPos;
    }

    auto exeNt = (IMAGE_NT_HEADERS*)((BYTE*)exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
    const bool isSb2 = (uintptr_t)exe == 0x400000 && exeNt->OptionalHeader.SizeOfImage > 0xE480;
    if (cfg.noCD && isSb2) {
        // sb2.exe 0x40E470 scans CD drives for Movies\mSpongeBob.bik. Make it return "found";
        // with no CD root recorded the game loads everything from its own folder.
        static const unsigned char expect[] = {0x81, 0xEC, 0xD8, 0x00, 0x00, 0x00, 0x53, 0x55};
        static const unsigned char patch[] = {0xB0, 0x01, 0xC3, 0x90, 0x90, 0x90, 0x53, 0x55};
        bool ok = PatchBytes((void*)0x0040E470, expect, "xxxxxxxx", patch, sizeof(patch));
        LOG_INFO("CD check bypass: %s", ok ? "applied" : "skipped (unexpected sb2.exe build)");
    }

    if (cfg.reduceCPU && omt) {
        // OMediaPeriodical::devote_time at OMT.dll+0x83090: mov edx,[abs32]; push esi (7 bytes).
        auto target = (unsigned char*)omt + 0x83090;
        if (target[0] == 0x8B && target[1] == 0x15 && target[6] == 0x56) {
            g_idleTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            g_origDevote = (DevoteFn)InstallDetour(target, (void*)DevoteTimeHook, 7);
            LOG_INFO("main-loop idling: %s", g_origDevote ? "enabled" : "failed");
        } else {
            LOG_INFO("main-loop idling: skipped (unexpected OMT.dll build)");
        }
    }
}

void OnFramePresented() {}

}  // namespace Compat
