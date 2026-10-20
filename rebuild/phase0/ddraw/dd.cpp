#include "ddraw_impl.h"
#include <algorithm>
#include <cstring>

DisplayMode g_mode;

static DDPIXELFORMAT DisplayFormat(int bpp) {
    PixelFormat f;
    if (bpp == 32) { f.bpp = 32; f.r = 0xFF0000; f.g = 0xFF00; f.b = 0xFF; }
    else if (bpp == 8) { f.bpp = 8; f.r = f.g = f.b = 0; }
    return f.ToDD();
}

DirectDraw::DirectDraw() {
    v1 = new DDv1; v1->impl = this;
    v4 = new DDv4; v4->impl = this;
    d3d = new D3D3; d3d->impl = this;
}

DirectDraw::~DirectDraw() {
    delete v1;
    delete v4;
    delete d3d;
}

ULONG DirectDraw::Release() {
    LONG r = --refs;
    if (r == 0) delete this;
    return (ULONG)r;
}

HRESULT DirectDraw::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirectDraw) *ppv = v1;
    else if (riid == IID_IDirectDraw4) *ppv = v4;
    else if (riid == IID_IDirect3D3) *ppv = d3d;
    else {
        LOG_INFO("DirectDraw QI for unsupported interface {%08lX-%04X-%04X}", riid.Data1, riid.Data2, riid.Data3);
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

HRESULT DirectDraw::SetCooperativeLevel(HWND w, DWORD flags) {
    coopFlags = flags;
    if (w) {
        hwnd = w;
        Renderer::AttachWindow(w);
    }
    return DD_OK;
}

static int MipCount(int w, int h) {
    int n = 1;
    while (w > 1 || h > 1) { w = std::max(1, w / 2); h = std::max(1, h / 2); ++n; }
    return n;
}

HRESULT DirectDraw::CreateSurface(const DDSURFACEDESC2& in, Surface** out) {
    if (!Renderer::Init()) return DDERR_GENERIC;
    DDSURFACEDESC2 d = in;
    d.dwSize = sizeof(d);
    DWORD c = d.ddsCaps.dwCaps;

    if (c & DDSCAPS_PRIMARYSURFACE) {
        d.dwWidth = g_mode.width;
        d.dwHeight = g_mode.height;
        d.dwFlags |= DDSD_WIDTH | DDSD_HEIGHT;
    } else if (!(d.dwFlags & DDSD_WIDTH) || !(d.dwFlags & DDSD_HEIGHT) || !d.dwWidth || !d.dwHeight) {
        return DDERR_INVALIDPARAMS;
    }

    if (!(d.dwFlags & DDSD_PIXELFORMAT)) {
        if (c & DDSCAPS_ZBUFFER) {
            PixelFormat z;
            z.zbuffer = true;
            // DDSURFACEDESC2 shares this union slot with the v1 dwZBufferBitDepth field.
            z.zbits = (d.dwFlags & DDSD_ZBUFFERBITDEPTH) && d.dwMipMapCount ? d.dwMipMapCount : 16;
            z.bpp = z.zbits == 24 ? 32 : (int)z.zbits;
            d.ddpfPixelFormat = z.ToDD();
        } else {
            d.ddpfPixelFormat = DisplayFormat(g_mode.bpp);
        }
        d.dwFlags |= DDSD_PIXELFORMAT;
    }

    // Memory placement the engine will see in GetCaps. Primary/back/z/texture report video memory
    // (the engine disables page flipping otherwise); plain offscreen surfaces report system memory,
    // which keeps the engine from maintaining a second system-memory cache of each one.
    const bool special = (c & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_ZBUFFER | DDSCAPS_TEXTURE)) != 0;
    if (!(c & (DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY))) {
        c |= special ? (DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM) : DDSCAPS_SYSTEMMEMORY;
    }
    if (c & DDSCAPS_VIDEOMEMORY) c |= DDSCAPS_LOCALVIDMEM;
    if (c & DDSCAPS_PRIMARYSURFACE) c |= DDSCAPS_VISIBLE | ((c & DDSCAPS_FLIP) ? DDSCAPS_FRONTBUFFER : 0);
    d.ddsCaps.dwCaps = c;

    Surface* s = new Surface(this, d);

    // Flip chain: one back buffer is all the engine requests.
    if ((c & DDSCAPS_PRIMARYSURFACE) && (c & DDSCAPS_FLIP) && (d.dwFlags & DDSD_BACKBUFFERCOUNT) && d.dwBackBufferCount) {
        DDSURFACEDESC2 bd = d;
        bd.dwFlags &= ~DDSD_BACKBUFFERCOUNT;
        bd.dwBackBufferCount = 0;
        bd.ddsCaps.dwCaps = (c & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE)) | DDSCAPS_BACKBUFFER;
        Surface* back = new Surface(this, bd);
        s->attached.push_back(back);  // the primary owns the back buffer's reference
    }

    // Mip chain: each level is its own surface attached to the previous one.
    if ((c & DDSCAPS_TEXTURE) && (c & DDSCAPS_MIPMAP)) {
        int levels = (d.dwFlags & DDSD_MIPMAPCOUNT) && d.dwMipMapCount ? (int)d.dwMipMapCount : MipCount(d.dwWidth, d.dwHeight);
        Surface* prev = s;
        int w = (int)d.dwWidth, h = (int)d.dwHeight;
        for (int level = 1; level < levels; ++level) {
            w = std::max(1, w / 2);
            h = std::max(1, h / 2);
            DDSURFACEDESC2 md = d;
            md.dwWidth = w;
            md.dwHeight = h;
            md.dwMipMapCount = levels - level;
            md.ddsCaps.dwCaps = c & ~DDSCAPS_COMPLEX;
            md.ddsCaps.dwCaps2 |= DDSCAPS2_MIPMAPSUBLEVEL;
            Surface* m = new Surface(this, md);
            m->parentMip = s;
            m->mipLevel = level;
            prev->attached.push_back(m);
            prev = m;
        }
    }

    *out = s;
    LOG_VERBOSE("CreateSurface %dx%d bpp=%d caps=%08lX", s->width, s->height, s->format.bpp, c);
    return DD_OK;
}

HRESULT DirectDraw::EnumDisplayModes(DWORD, LPDDSURFACEDESC2 filter, void* ctx, LPDDENUMMODESCALLBACK2 cb2, LPDDENUMMODESCALLBACK cb1) {
    static const int sizes[][2] = {{640, 480}, {800, 600}, {1024, 768}};
    static const int depths[] = {16, 32};
    for (int bpp : depths) {
        for (auto& sz : sizes) {
            DDSURFACEDESC2 d{};
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
            d.dwWidth = sz[0];
            d.dwHeight = sz[1];
            d.lPitch = sz[0] * bpp / 8;
            d.dwRefreshRate = 60;
            d.ddpfPixelFormat = DisplayFormat(bpp);
            if (filter) {
                if ((filter->dwFlags & DDSD_WIDTH) && filter->dwWidth != d.dwWidth) continue;
                if ((filter->dwFlags & DDSD_HEIGHT) && filter->dwHeight != d.dwHeight) continue;
                if ((filter->dwFlags & DDSD_PIXELFORMAT) && filter->ddpfPixelFormat.dwRGBBitCount != (DWORD)bpp) continue;
            }
            HRESULT r;
            if (cb2) {
                r = cb2(&d, ctx);
            } else {
                DDSURFACEDESC d1{};
                memcpy(&d1, &d, offsetof(DDSURFACEDESC, ddsCaps));
                d1.dwSize = sizeof(d1);
                r = cb1(&d1, ctx);
            }
            if (r == DDENUMRET_CANCEL) return DD_OK;
        }
    }
    return DD_OK;
}

void DirectDraw::GetCaps(DDCAPS* hw, DDCAPS* hel) {
    for (DDCAPS* c : {hw, hel}) {
        if (!c) continue;
        DWORD size = c->dwSize ? c->dwSize : sizeof(DDCAPS);
        memset(c, 0, size);
        c->dwSize = size;
        c->dwCaps = DDCAPS_3D | DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTDEPTHFILL | DDCAPS_BLTSTRETCH |
                    DDCAPS_COLORKEY | DDCAPS_CANBLTSYSMEM | DDCAPS_CANCLIP | DDCAPS_CANCLIPSTRETCHED;
        c->dwCaps2 = DDCAPS2_CANRENDERWINDOWED | DDCAPS2_FLIPNOVSYNC | DDCAPS2_NOPAGELOCKREQUIRED |
                     DDCAPS2_WIDESURFACES | DDCAPS2_PRIMARYGAMMA;
        c->dwCKeyCaps = DDCKEYCAPS_SRCBLT;
        c->dwFXCaps = DDFXCAPS_BLTSTRETCHX | DDFXCAPS_BLTSTRETCHY | DDFXCAPS_BLTSHRINKX | DDFXCAPS_BLTSHRINKY;
        c->dwZBufferBitDepths = DDBD_16 | DDBD_24 | DDBD_32;
        c->dwVidMemTotal = 256u << 20;
        c->dwVidMemFree = 240u << 20;
        c->ddsOldCaps.dwCaps = DDSCAPS_3DDEVICE | DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER |
                               DDSCAPS_OFFSCREENPLAIN | DDSCAPS_PRIMARYSURFACE | DDSCAPS_SYSTEMMEMORY |
                               DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY | DDSCAPS_ZBUFFER | DDSCAPS_MIPMAP |
                               DDSCAPS_LOCALVIDMEM | DDSCAPS_COMPLEX;
        if (size >= sizeof(DDCAPS_DX6)) ((DDCAPS_DX6*)c)->ddsCaps.dwCaps = c->ddsOldCaps.dwCaps;
    }
}

void DirectDraw::GetDisplayMode(DDSURFACEDESC2* d) {
    DWORD size = d->dwSize;
    memset(d, 0, size == sizeof(DDSURFACEDESC) ? sizeof(DDSURFACEDESC) : sizeof(DDSURFACEDESC2));
    d->dwSize = size;
    d->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    d->dwWidth = g_mode.width;
    d->dwHeight = g_mode.height;
    d->lPitch = g_mode.width * g_mode.bpp / 8;
    d->dwRefreshRate = 60;
    d->ddpfPixelFormat = DisplayFormat(g_mode.bpp);
}

static HRESULT SetMode(DWORD w, DWORD h, DWORD bpp) {
    if (bpp != 8 && bpp != 16 && bpp != 32) return DDERR_INVALIDMODE;
    g_mode = {(int)w, (int)h, (int)bpp};
    LOG_INFO("SetDisplayMode %lux%lux%lu (emulated; desktop mode unchanged)", w, h, bpp);
    return DD_OK;
}

static Clipper* NewClipper() { return new Clipper; }

// ---------------------------------------------------------------------------------------------
// IDirectDraw face
// ---------------------------------------------------------------------------------------------
#define D impl

HRESULT DDv1::QueryInterface(REFIID riid, void** ppv) { return D->QueryInterface(riid, ppv); }
ULONG DDv1::AddRef() { return D->AddRef(); }
ULONG DDv1::Release() { return D->Release(); }
HRESULT DDv1::Compact() { return DD_OK; }
HRESULT DDv1::CreateClipper(DWORD, LPDIRECTDRAWCLIPPER* c, IUnknown*) { if (!c) return DDERR_INVALIDPARAMS; *c = NewClipper(); return DD_OK; }
HRESULT DDv1::CreatePalette(DWORD flags, LPPALETTEENTRY entries, LPDIRECTDRAWPALETTE* out, IUnknown*) {
    if (!out) return DDERR_INVALIDPARAMS;
    Palette* p = new Palette;
    p->flags = flags;
    if (entries) memcpy(p->entries, entries, sizeof(p->entries));
    *out = p;
    return DD_OK;
}
HRESULT DDv1::CreateSurface(LPDDSURFACEDESC d1, LPDIRECTDRAWSURFACE* out, IUnknown*) {
    if (!d1 || !out) return DDERR_INVALIDPARAMS;
    DDSURFACEDESC2 d{};
    memcpy(&d, d1, offsetof(DDSURFACEDESC, ddsCaps));
    d.ddsCaps.dwCaps = d1->ddsCaps.dwCaps;
    Surface* s = nullptr;
    HRESULT hr = D->CreateSurface(d, &s);
    *out = s ? s->v1 : nullptr;
    return hr;
}
HRESULT DDv1::DuplicateSurface(LPDIRECTDRAWSURFACE, LPDIRECTDRAWSURFACE*) { LOG_UNIMPL("DuplicateSurface"); return DDERR_UNSUPPORTED; }
HRESULT DDv1::EnumDisplayModes(DWORD f, LPDDSURFACEDESC filter, LPVOID ctx, LPDDENUMMODESCALLBACK cb) {
    DDSURFACEDESC2 f2{};
    if (filter) { memcpy(&f2, filter, offsetof(DDSURFACEDESC, ddsCaps)); }
    return D->EnumDisplayModes(f, filter ? &f2 : nullptr, ctx, nullptr, cb);
}
HRESULT DDv1::EnumSurfaces(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMSURFACESCALLBACK) { LOG_UNIMPL("EnumSurfaces"); return DDERR_UNSUPPORTED; }
HRESULT DDv1::FlipToGDISurface() { return DD_OK; }
HRESULT DDv1::GetCaps(LPDDCAPS hw, LPDDCAPS hel) { D->GetCaps(hw, hel); return DD_OK; }
HRESULT DDv1::GetDisplayMode(LPDDSURFACEDESC d) {
    if (!d) return DDERR_INVALIDPARAMS;
    DDSURFACEDESC2 d2{};
    d2.dwSize = sizeof(d2);
    D->GetDisplayMode(&d2);
    memcpy(d, &d2, offsetof(DDSURFACEDESC, ddsCaps));
    d->dwSize = sizeof(DDSURFACEDESC);
    return DD_OK;
}
HRESULT DDv1::GetFourCCCodes(LPDWORD n, LPDWORD) { if (n) *n = 0; return DD_OK; }
HRESULT DDv1::GetGDISurface(LPDIRECTDRAWSURFACE*) { LOG_UNIMPL("GetGDISurface"); return DDERR_NOTFOUND; }
HRESULT DDv1::GetMonitorFrequency(LPDWORD f) { if (f) *f = 60; return DD_OK; }
HRESULT DDv1::GetScanLine(LPDWORD l) { if (l) *l = 0; return DDERR_VERTICALBLANKINPROGRESS; }
HRESULT DDv1::GetVerticalBlankStatus(LPBOOL b) { if (b) *b = TRUE; return DD_OK; }
HRESULT DDv1::Initialize(GUID*) { return DDERR_ALREADYINITIALIZED; }
HRESULT DDv1::RestoreDisplayMode() { return DD_OK; }
HRESULT DDv1::SetCooperativeLevel(HWND w, DWORD f) { return D->SetCooperativeLevel(w, f); }
HRESULT DDv1::SetDisplayMode(DWORD w, DWORD h, DWORD bpp) { return SetMode(w, h, bpp); }
HRESULT DDv1::WaitForVerticalBlank(DWORD, HANDLE) { return DD_OK; }

// ---------------------------------------------------------------------------------------------
// IDirectDraw4 face
// ---------------------------------------------------------------------------------------------
HRESULT DDv4::QueryInterface(REFIID riid, void** ppv) { return D->QueryInterface(riid, ppv); }
ULONG DDv4::AddRef() { return D->AddRef(); }
ULONG DDv4::Release() { return D->Release(); }
HRESULT DDv4::Compact() { return DD_OK; }
HRESULT DDv4::CreateClipper(DWORD f, LPDIRECTDRAWCLIPPER* c, IUnknown* u) { return D->v1->CreateClipper(f, c, u); }
HRESULT DDv4::CreatePalette(DWORD f, LPPALETTEENTRY e, LPDIRECTDRAWPALETTE* p, IUnknown* u) { return D->v1->CreatePalette(f, e, p, u); }
HRESULT DDv4::CreateSurface(LPDDSURFACEDESC2 d, LPDIRECTDRAWSURFACE4* out, IUnknown*) {
    if (!d || !out) return DDERR_INVALIDPARAMS;
    Surface* s = nullptr;
    HRESULT hr = D->CreateSurface(*d, &s);
    *out = s ? s->v4 : nullptr;
    return hr;
}
HRESULT DDv4::DuplicateSurface(LPDIRECTDRAWSURFACE4, LPDIRECTDRAWSURFACE4*) { LOG_UNIMPL("DuplicateSurface4"); return DDERR_UNSUPPORTED; }
HRESULT DDv4::EnumDisplayModes(DWORD f, LPDDSURFACEDESC2 filter, LPVOID ctx, LPDDENUMMODESCALLBACK2 cb) { return D->EnumDisplayModes(f, filter, ctx, cb, nullptr); }
HRESULT DDv4::EnumSurfaces(DWORD, LPDDSURFACEDESC2, LPVOID, LPDDENUMSURFACESCALLBACK2) { LOG_UNIMPL("EnumSurfaces4"); return DDERR_UNSUPPORTED; }
HRESULT DDv4::FlipToGDISurface() { return DD_OK; }
HRESULT DDv4::GetCaps(LPDDCAPS hw, LPDDCAPS hel) { D->GetCaps(hw, hel); return DD_OK; }
HRESULT DDv4::GetDisplayMode(LPDDSURFACEDESC2 d) { if (!d) return DDERR_INVALIDPARAMS; D->GetDisplayMode(d); return DD_OK; }
HRESULT DDv4::GetFourCCCodes(LPDWORD n, LPDWORD) { if (n) *n = 0; return DD_OK; }
HRESULT DDv4::GetGDISurface(LPDIRECTDRAWSURFACE4*) { LOG_UNIMPL("GetGDISurface4"); return DDERR_NOTFOUND; }
HRESULT DDv4::GetMonitorFrequency(LPDWORD f) { if (f) *f = 60; return DD_OK; }
HRESULT DDv4::GetScanLine(LPDWORD l) { if (l) *l = 0; return DDERR_VERTICALBLANKINPROGRESS; }
HRESULT DDv4::GetVerticalBlankStatus(LPBOOL b) { if (b) *b = TRUE; return DD_OK; }
HRESULT DDv4::Initialize(GUID*) { return DDERR_ALREADYINITIALIZED; }
HRESULT DDv4::RestoreDisplayMode() { return DD_OK; }
HRESULT DDv4::SetCooperativeLevel(HWND w, DWORD f) { return D->SetCooperativeLevel(w, f); }
HRESULT DDv4::SetDisplayMode(DWORD w, DWORD h, DWORD bpp, DWORD, DWORD) { return SetMode(w, h, bpp); }
HRESULT DDv4::WaitForVerticalBlank(DWORD, HANDLE) { return DD_OK; }
HRESULT DDv4::GetAvailableVidMem(LPDDSCAPS2, LPDWORD total, LPDWORD freeMem) {
    if (total) *total = 256u << 20;
    if (freeMem) *freeMem = 240u << 20;
    return DD_OK;
}
HRESULT DDv4::GetSurfaceFromDC(HDC, LPDIRECTDRAWSURFACE4*) { LOG_UNIMPL("GetSurfaceFromDC"); return DDERR_NOTFOUND; }
HRESULT DDv4::RestoreAllSurfaces() { return DD_OK; }
HRESULT DDv4::TestCooperativeLevel() { return DD_OK; }
HRESULT DDv4::GetDeviceIdentifier(LPDDDEVICEIDENTIFIER id, DWORD) {
    if (!id) return DDERR_INVALIDPARAMS;
    memset(id, 0, sizeof(*id));
    strcpy(id->szDriver, "sb2modern");
    strcpy(id->szDescription, "Direct3D 11 (sb2modern)");
    return DD_OK;
}

// ---------------------------------------------------------------------------------------------
// Clipper
// ---------------------------------------------------------------------------------------------
HRESULT Clipper::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirectDrawClipper) { *ppv = this; AddRef(); return S_OK; }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
ULONG Clipper::AddRef() { return ++refs; }
ULONG Clipper::Release() { LONG r = --refs; if (!r) delete this; return (ULONG)r; }
HRESULT Clipper::GetClipList(LPRECT, LPRGNDATA, LPDWORD size) { if (size) *size = 0; return DDERR_NOCLIPLIST; }
HRESULT Clipper::GetHWnd(HWND* w) { if (w) *w = hwnd; return DD_OK; }
HRESULT Clipper::Initialize(LPDIRECTDRAW, DWORD) { return DDERR_ALREADYINITIALIZED; }
HRESULT Clipper::IsClipListChanged(BOOL* b) { if (b) *b = FALSE; return DD_OK; }
HRESULT Clipper::SetClipList(LPRGNDATA, DWORD) { return DD_OK; }
HRESULT Clipper::SetHWnd(DWORD, HWND w) { hwnd = w; return DD_OK; }
