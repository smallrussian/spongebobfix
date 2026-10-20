#include "ddraw_impl.h"
#include <cstring>

HRESULT Palette::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirectDrawPalette) { *ppv = this; AddRef(); return S_OK; }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
ULONG Palette::AddRef() { return ++refs; }
ULONG Palette::Release() { LONG r = --refs; if (!r) delete this; return (ULONG)r; }
HRESULT Palette::GetCaps(LPDWORD caps) { if (caps) *caps = flags; return DD_OK; }
HRESULT Palette::GetEntries(DWORD, DWORD base, DWORD count, LPPALETTEENTRY out) {
    if (!out || base + count > 256) return DDERR_INVALIDPARAMS;
    memcpy(out, entries + base, count * sizeof(PALETTEENTRY));
    return DD_OK;
}
HRESULT Palette::Initialize(LPDIRECTDRAW, DWORD, LPPALETTEENTRY) { return DDERR_ALREADYINITIALIZED; }
HRESULT Palette::SetEntries(DWORD, DWORD base, DWORD count, LPPALETTEENTRY in) {
    if (!in || base + count > 256) return DDERR_INVALIDPARAMS;
    memcpy(entries + base, in, count * sizeof(PALETTEENTRY));
    return DD_OK;
}
