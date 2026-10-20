#include "ddraw_impl.h"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        LOG_INFO("sb2modern ddraw.dll loaded");
        Compat::Install();
    }
    return TRUE;
}

extern "C" {

HRESULT WINAPI DirectDrawCreate(GUID*, LPDIRECTDRAW* out, IUnknown*) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = (new DirectDraw)->v1;
    return DD_OK;
}

HRESULT WINAPI DirectDrawCreateEx(GUID*, LPVOID* out, REFIID iid, IUnknown*) {
    if (!out) return DDERR_INVALIDPARAMS;
    auto dd = new DirectDraw;
    HRESULT hr = dd->QueryInterface(iid, out);
    dd->Release();
    return hr;
}

HRESULT WINAPI DirectDrawCreateClipper(DWORD, LPDIRECTDRAWCLIPPER* out, IUnknown*) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = new Clipper;
    return DD_OK;
}

HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA cb, LPVOID ctx) {
    if (cb) cb(nullptr, (LPSTR)"Primary Display Driver", (LPSTR)"display", ctx);
    return DD_OK;
}

HRESULT WINAPI DirectDrawEnumerateW(LPDDENUMCALLBACKW cb, LPVOID ctx) {
    if (cb) cb(nullptr, (LPWSTR)L"Primary Display Driver", (LPWSTR)L"display", ctx);
    return DD_OK;
}

HRESULT WINAPI DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA cb, LPVOID ctx, DWORD) {
    if (cb) cb(nullptr, (LPSTR)"Primary Display Driver", (LPSTR)"display", ctx, nullptr);
    return DD_OK;
}

HRESULT WINAPI DirectDrawEnumerateExW(LPDDENUMCALLBACKEXW cb, LPVOID ctx, DWORD) {
    if (cb) cb(nullptr, (LPWSTR)L"Primary Display Driver", (LPWSTR)L"display", ctx, nullptr);
    return DD_OK;
}

}  // extern "C"
