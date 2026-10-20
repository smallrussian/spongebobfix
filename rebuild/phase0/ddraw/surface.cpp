#include "ddraw_impl.h"
#include <algorithm>
#include <cstring>

// ---------------------------------------------------------------------------------------------
// Pixel formats
// ---------------------------------------------------------------------------------------------
DDPIXELFORMAT PixelFormat::ToDD() const {
    DDPIXELFORMAT pf{};
    pf.dwSize = sizeof(pf);
    if (zbuffer) {
        pf.dwFlags = DDPF_ZBUFFER;
        pf.dwZBufferBitDepth = zbits;
        pf.dwZBitMask = zbits >= 32 ? 0xFFFFFFFF : ((1u << zbits) - 1);
        return pf;
    }
    if (bpp == 8) {
        pf.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
        pf.dwRGBBitCount = 8;
        return pf;
    }
    pf.dwFlags = DDPF_RGB | (a ? DDPF_ALPHAPIXELS : 0);
    pf.dwRGBBitCount = bpp;
    pf.dwRBitMask = r;
    pf.dwGBitMask = g;
    pf.dwBBitMask = b;
    pf.dwRGBAlphaBitMask = a;
    return pf;
}

PixelFormat PixelFormat::FromDD(const DDPIXELFORMAT& pf) {
    PixelFormat f;
    if (pf.dwFlags & DDPF_ZBUFFER) {
        f.zbuffer = true;
        f.zbits = pf.dwZBufferBitDepth ? pf.dwZBufferBitDepth : 16;
        f.bpp = f.zbits == 24 ? 32 : (int)f.zbits;
        f.r = f.g = f.b = f.a = 0;
        return f;
    }
    if (pf.dwFlags & DDPF_PALETTEINDEXED8) {
        f.bpp = 8;
        f.r = f.g = f.b = f.a = 0;
        return f;
    }
    f.bpp = pf.dwRGBBitCount ? (int)pf.dwRGBBitCount : 16;
    f.r = pf.dwRBitMask;
    f.g = pf.dwGBitMask;
    f.b = pf.dwBBitMask;
    f.a = (pf.dwFlags & DDPF_ALPHAPIXELS) ? pf.dwRGBAlphaBitMask : 0;
    if (!f.r && !f.g && !f.b) {  // unspecified: pick the conventional layout for the depth
        if (f.bpp == 16) { f.r = 0xF800; f.g = 0x07E0; f.b = 0x001F; }
        else { f.r = 0xFF0000; f.g = 0xFF00; f.b = 0xFF; }
    }
    return f;
}

static void MaskInfo(uint32_t mask, int& shift, int& bits) {
    shift = 0; bits = 0;
    if (!mask) return;
    while (!(mask & 1)) { mask >>= 1; ++shift; }
    while (mask & 1) { mask >>= 1; ++bits; }
}

ChannelDecoder::ChannelDecoder(const PixelFormat& pf) {
    mask[0] = pf.r; mask[1] = pf.g; mask[2] = pf.b; mask[3] = pf.a;
    for (int i = 0; i < 4; ++i) MaskInfo(mask[i], shift[i], bits[i]);
}

static inline uint32_t Expand(uint32_t v, int bits) {
    if (bits >= 8) return v >> (bits - 8);
    if (bits == 0) return 0;
    // Replicate high bits into low bits so full-scale values map to 255.
    uint32_t r = v << (8 - bits);
    for (int b = bits; b < 8; b += bits) r |= r >> bits;
    return r & 0xFF;
}

uint32_t ChannelDecoder::ToBGRA(uint32_t px) const {
    uint32_t c[4];
    for (int i = 0; i < 4; ++i) c[i] = bits[i] ? Expand((px & mask[i]) >> shift[i], bits[i]) : 255;
    return (c[3] << 24) | (c[0] << 16) | (c[1] << 8) | c[2];
}

uint32_t ChannelDecoder::FromBGRA(uint32_t argb) const {
    uint32_t c[4] = {(argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, argb >> 24};
    uint32_t px = 0;
    for (int i = 0; i < 4; ++i)
        if (bits[i]) px |= ((bits[i] >= 8 ? c[i] << (bits[i] - 8) : c[i] >> (8 - bits[i])) << shift[i]) & mask[i];
    return px;
}

static inline uint32_t ReadPixel(const uint8_t* p, int bpp) {
    switch (bpp) {
    case 8: return *p;
    case 16: return *(const uint16_t*)p;
    case 24: return p[0] | (p[1] << 8) | (p[2] << 16);
    default: return *(const uint32_t*)p;
    }
}

static inline void WritePixel(uint8_t* p, int bpp, uint32_t v) {
    switch (bpp) {
    case 8: *p = (uint8_t)v; break;
    case 16: *(uint16_t*)p = (uint16_t)v; break;
    case 24: p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); break;
    default: *(uint32_t*)p = v; break;
    }
}

// ---------------------------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------------------------
Storage::~Storage() {
    if (srv) srv->Release();
    if (rtv) rtv->Release();
    if (dsv) dsv->Release();
    if (tex) tex->Release();
    if (dc) {
        if (oldBitmap) SelectObject(dc, oldBitmap);
        DeleteDC(dc);
    }
    if (dib) DeleteObject(dib);
    else if (bits) _aligned_free(bits);
}

static std::unique_ptr<Storage> AllocStorage(int w, int h, int bpp) {
    auto st = std::make_unique<Storage>();
    st->pitch = ((w * (bpp / 8)) + 3) & ~3;
    size_t size = (size_t)st->pitch * h;
    st->bits = (uint8_t*)_aligned_malloc(size ? size : 4, 16);
    memset(st->bits, 0, size);
    return st;
}

// Moves the CPU copy into a DIB section so GDI can draw on it.
static bool PromoteToDib(Storage* st, const PixelFormat& f, int w, int h) {
    if (st->dib) return true;
    struct { BITMAPINFOHEADER h; DWORD masks[256]; } bi{};
    bi.h.biSize = sizeof(BITMAPINFOHEADER);
    bi.h.biWidth = w;
    bi.h.biHeight = -h;  // top-down
    bi.h.biPlanes = 1;
    bi.h.biBitCount = (WORD)f.bpp;
    if (f.bpp == 16 || f.bpp == 32) {
        bi.h.biCompression = BI_BITFIELDS;
        bi.masks[0] = f.r; bi.masks[1] = f.g; bi.masks[2] = f.b;
    } else if (f.bpp == 8) {
        bi.h.biClrUsed = 256;
        for (int i = 0; i < 256; ++i) bi.masks[i] = (i << 16) | (i << 8) | i;
    }
    void* dibBits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
    if (!dib) return false;
    memcpy(dibBits, st->bits, (size_t)st->pitch * h);
    _aligned_free(st->bits);
    st->bits = (uint8_t*)dibBits;
    st->dib = dib;
    st->dc = CreateCompatibleDC(nullptr);
    st->oldBitmap = SelectObject(st->dc, dib);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Surface
// ---------------------------------------------------------------------------------------------
Surface::Surface(DirectDraw* owner, const DDSURFACEDESC2& d) : dd(owner), desc(d) {
    v4 = new SurfaceV4; v4->impl = this;
    v1 = new SurfaceV1; v1->impl = this;
    tex2 = new TextureFace; tex2->impl = this;
    width = (int)d.dwWidth;
    height = (int)d.dwHeight;
    caps = d.ddsCaps.dwCaps;
    caps2 = d.ddsCaps.dwCaps2;
    format = PixelFormat::FromDD(d.ddpfPixelFormat);
    primary = (caps & DDSCAPS_PRIMARYSURFACE) != 0;
    backbuffer = (caps & DDSCAPS_BACKBUFFER) != 0;
    zbuffer = (caps & DDSCAPS_ZBUFFER) != 0;
    texture = (caps & DDSCAPS_TEXTURE) != 0;
    if (d.dwFlags & DDSD_CKSRCBLT) { hasSrcKey = true; srcKey = d.ddckCKSrcBlt; }
    store = AllocStorage(width, height, format.bpp);
    dd->AddRef();
}

Surface::~Surface() {
    for (Surface* a : attached) a->Release();
    if (palette) palette->Release();
    if (clipper) clipper->Release();
    DirectDraw* owner = dd;
    delete v4;
    delete v1;
    delete tex2;
    owner->Release();
}

ULONG Surface::Release() {
    LONG r = --refs;
    if (r == 0) {
        Renderer::Flush();
        delete this;
    }
    return (ULONG)r;
}

Surface* Surface::From(void* anyFace) {
    if (!anyFace) return nullptr;
    auto f = (Face<IUnknown, Surface>*)anyFace;
    if (f->magic != FaceMagic<Surface>()) return nullptr;
    return f->impl;
}

Surface* Surface::AttachedZ() const {
    for (Surface* a : attached)
        if (a->zbuffer) return a;
    return nullptr;
}

void Surface::EnsureGpu() {
    if (!store->tex) Renderer::CreateSurfaceResources(this);
}

void Surface::SyncToCpu() {
    if (store->where == Where::Gpu) {
        Renderer::Readback(this);
        store->where = Where::Both;
    }
}

void Surface::SyncToGpu() {
    EnsureGpu();
    if (store->where == Where::Cpu || (texture && TopMip()->textureDirty)) {
        Renderer::Upload(this);
        store->where = Where::Both;
    }
}

void Surface::FillDesc(DDSURFACEDESC2* out) const {
    DWORD size = out->dwSize;
    memset(out, 0, size == sizeof(DDSURFACEDESC) ? sizeof(DDSURFACEDESC) : sizeof(DDSURFACEDESC2));
    out->dwSize = size;
    out->dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    out->dwWidth = width;
    out->dwHeight = height;
    out->lPitch = store->pitch;
    out->ddpfPixelFormat = format.ToDD();
    if (hasSrcKey) { out->dwFlags |= DDSD_CKSRCBLT; out->ddckCKSrcBlt = srcKey; }
    if (desc.dwFlags & DDSD_BACKBUFFERCOUNT) { out->dwFlags |= DDSD_BACKBUFFERCOUNT; out->dwBackBufferCount = desc.dwBackBufferCount; }
    if (zbuffer) { out->dwFlags |= DDSD_ZBUFFERBITDEPTH; out->dwMipMapCount = format.zbits; }  // v1 dwZBufferBitDepth slot
    // v1 descriptors carry a 4-byte DDSCAPS at the same offset.
    if (size == sizeof(DDSURFACEDESC)) ((DDSURFACEDESC*)out)->ddsCaps.dwCaps = caps;
    else { out->ddsCaps.dwCaps = caps; out->ddsCaps.dwCaps2 = caps2; }
}

HRESULT Surface::Lock(RECT* rect, DDSURFACEDESC2* out, DWORD flags) {
    if (!out) return DDERR_INVALIDPARAMS;
    Renderer::Flush();
    SyncToCpu();
    FillDesc(out);
    out->dwFlags |= DDSD_LPSURFACE;
    uint8_t* p = store->bits;
    if (rect) p += rect->top * store->pitch + rect->left * (format.bpp / 8);
    out->lpSurface = p;
    ++lockCount;
    lockWrites = lockWrites || !(flags & DDLOCK_READONLY);
    return DD_OK;
}

HRESULT Surface::Unlock() {
    if (lockCount > 0) --lockCount;
    if (lockCount == 0 && lockWrites) {
        lockWrites = false;
        MarkCpuWritten();
        if (parentMip) parentMip->textureDirty = true;
        if (primary) Renderer::MarkPrimaryDirty(this);
    }
    return DD_OK;
}

// Clips a destination rectangle to the surface and shrinks the source to match (1:1 blits only).
static bool ClipRects(RECT& d, RECT& s, int dw, int dh, int sw, int sh, bool stretch) {
    if (!stretch) {
        if (d.left < 0) { s.left -= d.left; d.left = 0; }
        if (d.top < 0) { s.top -= d.top; d.top = 0; }
        if (s.left < 0) { d.left -= s.left; s.left = 0; }
        if (s.top < 0) { d.top -= s.top; s.top = 0; }
        LONG w = std::min(d.right - d.left, s.right - s.left);
        LONG h = std::min(d.bottom - d.top, s.bottom - s.top);
        w = std::min<LONG>(w, std::min<LONG>(dw - d.left, sw - s.left));
        h = std::min<LONG>(h, std::min<LONG>(dh - d.top, sh - s.top));
        if (w <= 0 || h <= 0) return false;
        d.right = d.left + w; d.bottom = d.top + h;
        s.right = s.left + w; s.bottom = s.top + h;
        return true;
    }
    // Stretch blits: only reject fully outside; per-pixel loop clips against the destination.
    return d.right > d.left && d.bottom > d.top && s.right > s.left && s.bottom > s.top;
}

static void CpuBlit(Surface* dst, RECT d, Surface* src, RECT s, const DDCOLORKEY* key) {
    const int dbpp = dst->format.bpp, sbpp = src->format.bpp;
    const int dbytes = dbpp / 8, sbytes = sbpp / 8;
    const bool sameFormat = dst->format == src->format;
    const LONG dw = d.right - d.left, dh = d.bottom - d.top;
    const LONG sw = s.right - s.left, sh = s.bottom - s.top;
    const bool stretch = dw != sw || dh != sh;
    uint8_t* dbits = dst->store->bits;
    const int dpitch = dst->store->pitch;

    // Overlapping copies within one surface go through a temporary buffer.
    std::vector<uint8_t> temp;
    const uint8_t* sbits = src->store->bits;
    int spitch = src->store->pitch;
    if (src == dst) {
        temp.assign(src->store->bits, src->store->bits + (size_t)spitch * src->height);
        sbits = temp.data();
    }

    if (sameFormat && !stretch && !key) {
        for (LONG y = 0; y < dh; ++y)
            memcpy(dbits + (d.top + y) * dpitch + d.left * dbytes,
                   sbits + (s.top + y) * spitch + s.left * sbytes, (size_t)dw * dbytes);
        return;
    }

    ChannelDecoder sdec(src->format), ddec(dst->format);
    const uint32_t klo = key ? key->dwColorSpaceLowValue : 0, khi = key ? key->dwColorSpaceHighValue : 0;
    for (LONG y = 0; y < dh; ++y) {
        LONG dy = d.top + y;
        if (dy < 0 || dy >= dst->height) continue;
        LONG sy = s.top + (stretch ? (y * sh) / dh : y);
        const uint8_t* srow = sbits + sy * spitch;
        uint8_t* drow = dbits + dy * dpitch;
        for (LONG x = 0; x < dw; ++x) {
            LONG dx = d.left + x;
            if (dx < 0 || dx >= dst->width) continue;
            LONG sx = s.left + (stretch ? (x * sw) / dw : x);
            uint32_t px = ReadPixel(srow + sx * sbytes, sbpp);
            if (key && px >= klo && px <= khi) continue;
            if (!sameFormat) px = ddec.FromBGRA(sdec.ToBGRA(px));
            WritePixel(drow + dx * dbytes, dbpp, px);
        }
    }
}

static void CpuFill(Surface* dst, const RECT& d, uint32_t color) {
    const int bytes = dst->format.bpp / 8;
    for (LONG y = d.top; y < d.bottom; ++y) {
        uint8_t* row = dst->store->bits + y * dst->store->pitch + d.left * bytes;
        if (bytes == 2) std::fill((uint16_t*)row, (uint16_t*)row + (d.right - d.left), (uint16_t)color);
        else if (bytes == 4) std::fill((uint32_t*)row, (uint32_t*)row + (d.right - d.left), color);
        else for (LONG x = d.left; x < d.right; ++x, row += bytes) WritePixel(row, dst->format.bpp, color);
    }
}

HRESULT Surface::Blt(RECT* dstRect, Surface* src, RECT* srcRect, DWORD flags, DDBLTFX* fx) {
    RECT d = dstRect ? *dstRect : RECT{0, 0, width, height};
    Renderer::Flush();

    if (flags & DDBLT_DEPTHFILL) {
        if (zbuffer && fx) {
            float z = format.zbits >= 32 ? fx->dwFillDepth / 4294967295.0f : fx->dwFillDepth / (float)((1u << format.zbits) - 1);
            Renderer::ClearRect(nullptr, this, d, D3DCLEAR_ZBUFFER, 0, z);
        }
        return DD_OK;
    }
    if (flags & DDBLT_COLORFILL) {
        if (!fx) return DDERR_INVALIDPARAMS;
        d.left = std::max<LONG>(d.left, 0); d.top = std::max<LONG>(d.top, 0);
        d.right = std::min<LONG>(d.right, width); d.bottom = std::min<LONG>(d.bottom, height);
        if (d.right <= d.left || d.bottom <= d.top) return DD_OK;
        SyncToCpu();
        CpuFill(this, d, fx->dwFillColor);
        MarkCpuWritten();
        if (primary) Renderer::MarkPrimaryDirty(this);
        return DD_OK;
    }
    if (!src) return DDERR_INVALIDPARAMS;

    RECT s = srcRect ? *srcRect : RECT{0, 0, src->width, src->height};
    bool stretch = (d.right - d.left) != (s.right - s.left) || (d.bottom - d.top) != (s.bottom - s.top);
    if (!ClipRects(d, s, width, height, src->width, src->height, stretch)) return DD_OK;

    const DDCOLORKEY* key = nullptr;
    if ((flags & DDBLT_KEYSRCOVERRIDE) && fx) key = &fx->ddckSrcColorkey;
    else if ((flags & DDBLT_KEYSRC) && src->hasSrcKey) key = &src->srcKey;
    if (flags & DDBLT_DDFX) LOG_UNIMPL("Blt with DDBLT_DDFX effects");

    src->SyncToCpu();
    SyncToCpu();
    CpuBlit(this, d, src, s, key);
    MarkCpuWritten();
    if (parentMip) parentMip->textureDirty = true;
    if (primary) Renderer::MarkPrimaryDirty(this);
    return DD_OK;
}

HRESULT Surface::BltFast(DWORD x, DWORD y, Surface* src, RECT* srcRect, DWORD flags) {
    if (!src) return DDERR_INVALIDPARAMS;
    RECT s = srcRect ? *srcRect : RECT{0, 0, src->width, src->height};
    RECT d{(LONG)x, (LONG)y, (LONG)x + (s.right - s.left), (LONG)y + (s.bottom - s.top)};
    DWORD bltFlags = 0;
    if ((flags & DDBLTFAST_SRCCOLORKEY) && src->hasSrcKey) bltFlags |= DDBLT_KEYSRC;
    if (flags & DDBLTFAST_DESTCOLORKEY) LOG_UNIMPL("BltFast with DDBLTFAST_DESTCOLORKEY");
    return Blt(&d, src, &s, bltFlags, nullptr);
}

HRESULT Surface::Flip(Surface* target, DWORD) {
    Surface* back = target;
    if (!back)
        for (Surface* a : attached)
            if (a->backbuffer) { back = a; break; }
    if (!back) return DDERR_NOTFLIPPABLE;
    Renderer::Flush();
    // Real DirectDraw flips swap memory, leaving the back buffer holding the frame from two
    // flips ago. The game's movie player writes only changed Bink blocks into the back buffer,
    // which then shows stale blocks; the engine itself redraws whatever it considers stale on
    // either page. Presenting the back buffer and keeping its contents (copy semantics) is
    // correct for both.
    ++uniqueness;
    Renderer::Present(back);
    return DD_OK;
}

HRESULT Surface::GetAttached(DWORD wantCaps, Surface** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    for (Surface* a : attached) {
        if ((a->caps & wantCaps) == wantCaps) {
            a->AddRef();
            *out = a;
            return DD_OK;
        }
    }
    *out = nullptr;
    return DDERR_NOTFOUND;
}

HRESULT Surface::GetDC(HDC* out) {
    if (!out) return DDERR_INVALIDPARAMS;
    Renderer::Flush();
    SyncToCpu();
    if (!PromoteToDib(store.get(), format, width, height)) return DDERR_GENERIC;
    GdiFlush();
    *out = store->dc;
    ++lockCount;
    lockWrites = true;
    return DD_OK;
}

HRESULT Surface::ReleaseDC(HDC) {
    GdiFlush();
    return Unlock();
}

// ---------------------------------------------------------------------------------------------
// IDirectDrawSurface4 face
// ---------------------------------------------------------------------------------------------
#define S impl

HRESULT SurfaceV4::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirectDrawSurface4) { *ppv = this; }
    else if (riid == IID_IDirectDrawSurface) { *ppv = S->v1; }
    else if (riid == IID_IDirect3DTexture2) { *ppv = S->tex2; }
    else {
        LOG_VERBOSE("Surface QI for unsupported interface %08lX", riid.Data1);
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    S->AddRef();
    return S_OK;
}
ULONG SurfaceV4::AddRef() { return S->AddRef(); }
ULONG SurfaceV4::Release() { return S->Release(); }

HRESULT SurfaceV4::AddAttachedSurface(LPDIRECTDRAWSURFACE4 p) {
    Surface* a = Surface::From(p);
    if (!a) return DDERR_INVALIDPARAMS;
    a->AddRef();
    S->attached.push_back(a);
    return DD_OK;
}
HRESULT SurfaceV4::AddOverlayDirtyRect(LPRECT) { LOG_UNIMPL("AddOverlayDirtyRect"); return DDERR_UNSUPPORTED; }
HRESULT SurfaceV4::Blt(LPRECT d, LPDIRECTDRAWSURFACE4 src, LPRECT s, DWORD flags, LPDDBLTFX fx) {
    return S->Blt(d, Surface::From(src), s, flags, fx);
}
HRESULT SurfaceV4::BltBatch(LPDDBLTBATCH, DWORD, DWORD) { LOG_UNIMPL("BltBatch"); return DDERR_UNSUPPORTED; }
HRESULT SurfaceV4::BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE4 src, LPRECT s, DWORD flags) {
    return S->BltFast(x, y, Surface::From(src), s, flags);
}
HRESULT SurfaceV4::DeleteAttachedSurface(DWORD, LPDIRECTDRAWSURFACE4 p) {
    Surface* which = Surface::From(p);
    auto& v = S->attached;
    for (auto it = v.begin(); it != v.end();) {
        // Detaching "all" keeps the flip chain intact; the engine only means its z-buffer.
        if (which ? *it == which : !(*it)->backbuffer) { (*it)->Release(); it = v.erase(it); }
        else ++it;
    }
    return DD_OK;
}
HRESULT SurfaceV4::EnumAttachedSurfaces(LPVOID ctx, LPDDENUMSURFACESCALLBACK2 cb) {
    for (Surface* a : S->attached) {
        DDSURFACEDESC2 d{};
        d.dwSize = sizeof(d);
        a->FillDesc(&d);
        a->AddRef();
        if (cb(a->v4, &d, ctx) == DDENUMRET_CANCEL) break;
    }
    return DD_OK;
}
HRESULT SurfaceV4::EnumOverlayZOrders(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK2) { return DDERR_UNSUPPORTED; }
HRESULT SurfaceV4::Flip(LPDIRECTDRAWSURFACE4 t, DWORD f) { return S->Flip(Surface::From(t), f); }
HRESULT SurfaceV4::GetAttachedSurface(LPDDSCAPS2 c, LPDIRECTDRAWSURFACE4* out) {
    if (!c || !out) return DDERR_INVALIDPARAMS;
    Surface* a = nullptr;
    HRESULT hr = S->GetAttached(c->dwCaps, &a);
    *out = a ? a->v4 : nullptr;
    return hr;
}
HRESULT SurfaceV4::GetBltStatus(DWORD) { return DD_OK; }
HRESULT SurfaceV4::GetCaps(LPDDSCAPS2 c) {
    if (!c) return DDERR_INVALIDPARAMS;
    *c = {};
    c->dwCaps = S->caps;
    c->dwCaps2 = S->caps2;
    return DD_OK;
}
HRESULT SurfaceV4::GetClipper(LPDIRECTDRAWCLIPPER* c) {
    if (!c) return DDERR_INVALIDPARAMS;
    *c = S->clipper;
    if (!*c) return DDERR_NOCLIPPERATTACHED;
    (*c)->AddRef();
    return DD_OK;
}
HRESULT SurfaceV4::GetColorKey(DWORD flags, LPDDCOLORKEY k) {
    if (!k) return DDERR_INVALIDPARAMS;
    if ((flags & DDCKEY_SRCBLT) && S->hasSrcKey) { *k = S->srcKey; return DD_OK; }
    return DDERR_NOCOLORKEY;
}
HRESULT SurfaceV4::GetDC(HDC* dc) { return S->GetDC(dc); }
HRESULT SurfaceV4::GetFlipStatus(DWORD) { return DD_OK; }
HRESULT SurfaceV4::GetOverlayPosition(LPLONG, LPLONG) { return DDERR_NOTAOVERLAYSURFACE; }
HRESULT SurfaceV4::GetPalette(LPDIRECTDRAWPALETTE* p) {
    if (!p) return DDERR_INVALIDPARAMS;
    *p = S->palette;
    if (!*p) return DDERR_NOPALETTEATTACHED;
    (*p)->AddRef();
    return DD_OK;
}
HRESULT SurfaceV4::GetPixelFormat(LPDDPIXELFORMAT pf) {
    if (!pf) return DDERR_INVALIDPARAMS;
    *pf = S->format.ToDD();
    return DD_OK;
}
HRESULT SurfaceV4::GetSurfaceDesc(LPDDSURFACEDESC2 d) {
    if (!d) return DDERR_INVALIDPARAMS;
    S->FillDesc(d);
    return DD_OK;
}
HRESULT SurfaceV4::Initialize(LPDIRECTDRAW, LPDDSURFACEDESC2) { return DDERR_ALREADYINITIALIZED; }
HRESULT SurfaceV4::IsLost() { return DD_OK; }
HRESULT SurfaceV4::Lock(LPRECT r, LPDDSURFACEDESC2 d, DWORD flags, HANDLE) { return S->Lock(r, d, flags); }
HRESULT SurfaceV4::ReleaseDC(HDC dc) { return S->ReleaseDC(dc); }
HRESULT SurfaceV4::Restore() { return DD_OK; }
HRESULT SurfaceV4::SetClipper(LPDIRECTDRAWCLIPPER c) {
    if (c) c->AddRef();
    if (S->clipper) S->clipper->Release();
    S->clipper = c;
    return DD_OK;
}
HRESULT SurfaceV4::SetColorKey(DWORD flags, LPDDCOLORKEY k) {
    if (flags & DDCKEY_SRCBLT) {
        S->hasSrcKey = k != nullptr;
        if (k) {
            S->srcKey = *k;
            if (!(flags & DDCKEY_COLORSPACE)) S->srcKey.dwColorSpaceHighValue = k->dwColorSpaceLowValue;
        }
        S->TopMip()->textureDirty = true;
        return DD_OK;
    }
    LOG_VERBOSE("SetColorKey flags %08lX ignored", flags);
    return DD_OK;
}
HRESULT SurfaceV4::SetOverlayPosition(LONG, LONG) { return DDERR_NOTAOVERLAYSURFACE; }
HRESULT SurfaceV4::SetPalette(LPDIRECTDRAWPALETTE p) {
    Palette* np = p ? ((Palette*)((Face<IUnknown, Palette>*)p)->impl) : nullptr;
    if (np) np->AddRef();
    if (S->palette) S->palette->Release();
    S->palette = np;
    return DD_OK;
}
HRESULT SurfaceV4::Unlock(LPRECT) { return S->Unlock(); }
HRESULT SurfaceV4::UpdateOverlay(LPRECT, LPDIRECTDRAWSURFACE4, LPRECT, DWORD, LPDDOVERLAYFX) { return DDERR_NOTAOVERLAYSURFACE; }
HRESULT SurfaceV4::UpdateOverlayDisplay(DWORD) { return DDERR_NOTAOVERLAYSURFACE; }
HRESULT SurfaceV4::UpdateOverlayZOrder(DWORD, LPDIRECTDRAWSURFACE4) { return DDERR_NOTAOVERLAYSURFACE; }
HRESULT SurfaceV4::GetDDInterface(LPVOID* out) {
    if (!out) return DDERR_INVALIDPARAMS;
    S->dd->AddRef();
    *out = S->dd->v1;
    return DD_OK;
}
HRESULT SurfaceV4::PageLock(DWORD) { return DD_OK; }
HRESULT SurfaceV4::PageUnlock(DWORD) { return DD_OK; }
HRESULT SurfaceV4::SetSurfaceDesc(LPDDSURFACEDESC2, DWORD) { LOG_UNIMPL("SetSurfaceDesc"); return DDERR_UNSUPPORTED; }
HRESULT SurfaceV4::SetPrivateData(REFGUID, LPVOID, DWORD, DWORD) { return DDERR_UNSUPPORTED; }
HRESULT SurfaceV4::GetPrivateData(REFGUID, LPVOID, LPDWORD) { return DDERR_NOTFOUND; }
HRESULT SurfaceV4::FreePrivateData(REFGUID) { return DDERR_NOTFOUND; }
HRESULT SurfaceV4::GetUniquenessValue(LPDWORD v) { if (v) *v = S->uniqueness; return DD_OK; }
HRESULT SurfaceV4::ChangeUniquenessValue() { ++S->uniqueness; return DD_OK; }

// ---------------------------------------------------------------------------------------------
// IDirectDrawSurface (v1) face
// ---------------------------------------------------------------------------------------------
static void DescToV1(const DDSURFACEDESC2& d2, DDSURFACEDESC* d1) {
    // Identical layout up to ddsCaps; v1 has a 4-byte DDSCAPS.
    memcpy(d1, &d2, offsetof(DDSURFACEDESC, ddsCaps));
    d1->dwSize = sizeof(DDSURFACEDESC);
    d1->ddsCaps.dwCaps = d2.ddsCaps.dwCaps;
}

HRESULT SurfaceV1::QueryInterface(REFIID riid, void** ppv) { return S->v4->QueryInterface(riid, ppv); }
ULONG SurfaceV1::AddRef() { return S->AddRef(); }
ULONG SurfaceV1::Release() { return S->Release(); }
HRESULT SurfaceV1::AddAttachedSurface(LPDIRECTDRAWSURFACE p) { return S->v4->AddAttachedSurface((LPDIRECTDRAWSURFACE4)p); }
HRESULT SurfaceV1::AddOverlayDirtyRect(LPRECT r) { return S->v4->AddOverlayDirtyRect(r); }
HRESULT SurfaceV1::Blt(LPRECT d, LPDIRECTDRAWSURFACE src, LPRECT s, DWORD f, LPDDBLTFX fx) { return S->Blt(d, Surface::From(src), s, f, fx); }
HRESULT SurfaceV1::BltBatch(LPDDBLTBATCH b, DWORD c, DWORD f) { return S->v4->BltBatch(b, c, f); }
HRESULT SurfaceV1::BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE src, LPRECT s, DWORD f) { return S->BltFast(x, y, Surface::From(src), s, f); }
HRESULT SurfaceV1::DeleteAttachedSurface(DWORD f, LPDIRECTDRAWSURFACE p) { return S->v4->DeleteAttachedSurface(f, (LPDIRECTDRAWSURFACE4)p); }
HRESULT SurfaceV1::EnumAttachedSurfaces(LPVOID ctx, LPDDENUMSURFACESCALLBACK cb) {
    for (Surface* a : S->attached) {
        DDSURFACEDESC2 d2{};
        d2.dwSize = sizeof(d2);
        a->FillDesc(&d2);
        DDSURFACEDESC d1;
        DescToV1(d2, &d1);
        a->AddRef();
        if (cb(a->v1, &d1, ctx) == DDENUMRET_CANCEL) break;
    }
    return DD_OK;
}
HRESULT SurfaceV1::EnumOverlayZOrders(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK) { return DDERR_UNSUPPORTED; }
HRESULT SurfaceV1::Flip(LPDIRECTDRAWSURFACE t, DWORD f) { return S->Flip(Surface::From(t), f); }
HRESULT SurfaceV1::GetAttachedSurface(LPDDSCAPS c, LPDIRECTDRAWSURFACE* out) {
    if (!c || !out) return DDERR_INVALIDPARAMS;
    Surface* a = nullptr;
    HRESULT hr = S->GetAttached(c->dwCaps, &a);
    *out = a ? a->v1 : nullptr;
    return hr;
}
HRESULT SurfaceV1::GetBltStatus(DWORD) { return DD_OK; }
HRESULT SurfaceV1::GetCaps(LPDDSCAPS c) { if (!c) return DDERR_INVALIDPARAMS; c->dwCaps = S->caps; return DD_OK; }
HRESULT SurfaceV1::GetClipper(LPDIRECTDRAWCLIPPER* c) { return S->v4->GetClipper(c); }
HRESULT SurfaceV1::GetColorKey(DWORD f, LPDDCOLORKEY k) { return S->v4->GetColorKey(f, k); }
HRESULT SurfaceV1::GetDC(HDC* dc) { return S->GetDC(dc); }
HRESULT SurfaceV1::GetFlipStatus(DWORD) { return DD_OK; }
HRESULT SurfaceV1::GetOverlayPosition(LPLONG, LPLONG) { return DDERR_NOTAOVERLAYSURFACE; }
HRESULT SurfaceV1::GetPalette(LPDIRECTDRAWPALETTE* p) { return S->v4->GetPalette(p); }
HRESULT SurfaceV1::GetPixelFormat(LPDDPIXELFORMAT pf) { return S->v4->GetPixelFormat(pf); }
HRESULT SurfaceV1::GetSurfaceDesc(LPDDSURFACEDESC d) {
    if (!d) return DDERR_INVALIDPARAMS;
    DDSURFACEDESC2 d2{};
    d2.dwSize = sizeof(d2);
    S->FillDesc(&d2);
    DescToV1(d2, d);
    return DD_OK;
}
HRESULT SurfaceV1::Initialize(LPDIRECTDRAW, LPDDSURFACEDESC) { return DDERR_ALREADYINITIALIZED; }
HRESULT SurfaceV1::IsLost() { return DD_OK; }
HRESULT SurfaceV1::Lock(LPRECT r, LPDDSURFACEDESC d, DWORD flags, HANDLE) {
    if (!d) return DDERR_INVALIDPARAMS;
    DDSURFACEDESC2 d2{};
    d2.dwSize = sizeof(d2);
    HRESULT hr = S->Lock(r, &d2, flags);
    if (SUCCEEDED(hr)) DescToV1(d2, d);
    return hr;
}
HRESULT SurfaceV1::ReleaseDC(HDC dc) { return S->ReleaseDC(dc); }
HRESULT SurfaceV1::Restore() { return DD_OK; }
HRESULT SurfaceV1::SetClipper(LPDIRECTDRAWCLIPPER c) { return S->v4->SetClipper(c); }
HRESULT SurfaceV1::SetColorKey(DWORD f, LPDDCOLORKEY k) { return S->v4->SetColorKey(f, k); }
HRESULT SurfaceV1::SetOverlayPosition(LONG, LONG) { return DDERR_NOTAOVERLAYSURFACE; }
HRESULT SurfaceV1::SetPalette(LPDIRECTDRAWPALETTE p) { return S->v4->SetPalette(p); }
HRESULT SurfaceV1::Unlock(LPVOID) { return S->Unlock(); }
HRESULT SurfaceV1::UpdateOverlay(LPRECT, LPDIRECTDRAWSURFACE, LPRECT, DWORD, LPDDOVERLAYFX) { return DDERR_NOTAOVERLAYSURFACE; }
HRESULT SurfaceV1::UpdateOverlayDisplay(DWORD) { return DDERR_NOTAOVERLAYSURFACE; }
HRESULT SurfaceV1::UpdateOverlayZOrder(DWORD, LPDIRECTDRAWSURFACE) { return DDERR_NOTAOVERLAYSURFACE; }

// ---------------------------------------------------------------------------------------------
// IDirect3DTexture2 face
// ---------------------------------------------------------------------------------------------
HRESULT TextureFace::QueryInterface(REFIID riid, void** ppv) { return S->v4->QueryInterface(riid, ppv); }
ULONG TextureFace::AddRef() { return S->AddRef(); }
ULONG TextureFace::Release() { return S->Release(); }
HRESULT TextureFace::GetHandle(LPDIRECT3DDEVICE2, LPD3DTEXTUREHANDLE h) {
    if (h) *h = (D3DTEXTUREHANDLE)(uintptr_t)S;
    return D3D_OK;
}
HRESULT TextureFace::PaletteChanged(DWORD, DWORD) { S->textureDirty = true; return D3D_OK; }
HRESULT TextureFace::Load(LPDIRECT3DTEXTURE2 srcTex) {
    Surface* src = Surface::From(srcTex);
    if (!src) return DDERR_INVALIDPARAMS;
    // Copies each mip level of the source chain into this chain.
    Surface* d = S;
    Surface* s = src;
    while (d && s) {
        d->Blt(nullptr, s, nullptr, 0, nullptr);
        if (s->hasSrcKey) { d->hasSrcKey = true; d->srcKey = s->srcKey; }
        Surface* dn = nullptr;
        Surface* sn = nullptr;
        for (Surface* a : d->attached) if (a->caps & DDSCAPS_MIPMAP) { dn = a; break; }
        for (Surface* a : s->attached) if (a->caps & DDSCAPS_MIPMAP) { sn = a; break; }
        d = dn;
        s = sn;
    }
    S->textureDirty = true;
    return D3D_OK;
}
