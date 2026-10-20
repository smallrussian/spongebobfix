#include "ddraw_impl.h"
#include <cstring>

// Capabilities reported for our single "HAL" device: everything the engine probes for.
void FillDeviceDesc(D3DDEVICEDESC* d) {
    DWORD size = d->dwSize ? d->dwSize : sizeof(D3DDEVICEDESC);
    memset(d, 0, size);
    d->dwSize = size;
    d->dwFlags = D3DDD_COLORMODEL | D3DDD_DEVCAPS | D3DDD_TRANSFORMCAPS | D3DDD_LIGHTINGCAPS | D3DDD_BCLIPPING |
                 D3DDD_LINECAPS | D3DDD_TRICAPS | D3DDD_DEVICERENDERBITDEPTH | D3DDD_DEVICEZBUFFERBITDEPTH |
                 D3DDD_MAXBUFFERSIZE | D3DDD_MAXVERTEXCOUNT;
    d->dcmColorModel = D3DCOLOR_RGB;
    d->dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
                   D3DDEVCAPS_TEXTUREVIDEOMEMORY | D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP |
                   D3DDEVCAPS_TEXTURENONLOCALVIDMEM | D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_HWRASTERIZATION;
    d->dtcTransformCaps.dwSize = sizeof(D3DTRANSFORMCAPS);
    d->dtcTransformCaps.dwCaps = D3DTRANSFORMCAPS_CLIP;
    d->bClipping = TRUE;
    d->dlcLightingCaps.dwSize = sizeof(D3DLIGHTINGCAPS);
    d->dlcLightingCaps.dwCaps = D3DLIGHTCAPS_POINT | D3DLIGHTCAPS_SPOT | D3DLIGHTCAPS_DIRECTIONAL;
    d->dlcLightingCaps.dwLightingModel = D3DLIGHTINGMODEL_RGB;
    d->dlcLightingCaps.dwNumLights = 8;

    for (D3DPRIMCAPS* p : {&d->dpcLineCaps, &d->dpcTriCaps}) {
        p->dwSize = sizeof(D3DPRIMCAPS);
        p->dwMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW;
        p->dwRasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_SUBPIXEL |
                          D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE | D3DPRASTERCAPS_MIPMAPLODBIAS |
                          D3DPRASTERCAPS_ZBIAS | D3DPRASTERCAPS_ANISOTROPY | D3DPRASTERCAPS_WFOG |
                          D3DPRASTERCAPS_FOGRANGE | D3DPRASTERCAPS_ANTIALIASEDGES;
        p->dwZCmpCaps = 0xFF;
        p->dwSrcBlendCaps = 0x1FFF;
        p->dwDestBlendCaps = 0x1FFF;
        p->dwAlphaCmpCaps = 0xFF;
        p->dwShadeCaps = 0x000FFFFF;
        p->dwTextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_TRANSPARENCY |
                           D3DPTEXTURECAPS_BORDER | D3DPTEXTURECAPS_ALPHAPALETTE;
        p->dwTextureFilterCaps = D3DPTFILTERCAPS_NEAREST | D3DPTFILTERCAPS_LINEAR | D3DPTFILTERCAPS_MIPNEAREST |
                                 D3DPTFILTERCAPS_MIPLINEAR | D3DPTFILTERCAPS_LINEARMIPNEAREST |
                                 D3DPTFILTERCAPS_LINEARMIPLINEAR | D3DPTFILTERCAPS_MINFPOINT |
                                 D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MINFANISOTROPIC |
                                 D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR |
                                 D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;
        p->dwTextureBlendCaps = 0xFF;
        p->dwTextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP |
                                  D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_INDEPENDENTUV;
    }
    d->dwDeviceRenderBitDepth = DDBD_16 | DDBD_32;
    d->dwDeviceZBufferBitDepth = DDBD_16 | DDBD_24 | DDBD_32;
    d->dwMaxVertexCount = 65535;
    if (size >= sizeof(D3DDEVICEDESC)) {
        d->dwMinTextureWidth = d->dwMinTextureHeight = 1;
        d->dwMaxTextureWidth = d->dwMaxTextureHeight = 4096;
        d->dwMaxTextureRepeat = 8192;
        d->dwMaxTextureAspectRatio = 4096;
        d->dwMaxAnisotropy = 16;
        d->dvGuardBandLeft = d->dvGuardBandTop = -32768.0f;
        d->dvGuardBandRight = d->dvGuardBandBottom = 32767.0f;
        d->dwFVFCaps = 2;
        d->dwTextureOpCaps = 0x00FFFFFF;
        d->wMaxTextureBlendStages = 2;
        d->wMaxSimultaneousTextures = 2;
    }
}

// ---------------------------------------------------------------------------------------------
// IDirect3D3 face of the DirectDraw object
// ---------------------------------------------------------------------------------------------
#define D impl

HRESULT D3D3::QueryInterface(REFIID riid, void** ppv) { return D->QueryInterface(riid, ppv); }
ULONG D3D3::AddRef() { return D->AddRef(); }
ULONG D3D3::Release() { return D->Release(); }

HRESULT D3D3::EnumDevices(LPD3DENUMDEVICESCALLBACK cb, LPVOID ctx) {
    if (!cb) return DDERR_INVALIDPARAMS;
    D3DDEVICEDESC hw{}, hel{};
    hw.dwSize = hel.dwSize = sizeof(D3DDEVICEDESC);
    FillDeviceDesc(&hw);
    FillDeviceDesc(&hel);
    GUID g = IID_IDirect3DHALDevice;
    cb(&g, (LPSTR)"Direct3D HAL", (LPSTR)"Direct3D HAL", &hw, &hel, ctx);
    return D3D_OK;
}
HRESULT D3D3::CreateLight(LPDIRECT3DLIGHT*, LPUNKNOWN) { LOG_UNIMPL("CreateLight"); return DDERR_UNSUPPORTED; }
HRESULT D3D3::CreateMaterial(LPDIRECT3DMATERIAL3*, LPUNKNOWN) { LOG_UNIMPL("CreateMaterial"); return DDERR_UNSUPPORTED; }
HRESULT D3D3::CreateViewport(LPDIRECT3DVIEWPORT3* out, LPUNKNOWN) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = new Viewport;
    return D3D_OK;
}
HRESULT D3D3::FindDevice(LPD3DFINDDEVICESEARCH, LPD3DFINDDEVICERESULT r) {
    if (!r) return DDERR_INVALIDPARAMS;
    r->guid = IID_IDirect3DHALDevice;
    r->ddHwDesc.dwSize = r->ddSwDesc.dwSize = sizeof(D3DDEVICEDESC);
    FillDeviceDesc(&r->ddHwDesc);
    FillDeviceDesc(&r->ddSwDesc);
    return D3D_OK;
}
HRESULT D3D3::CreateDevice(REFCLSID, LPDIRECTDRAWSURFACE4 surf, LPDIRECT3DDEVICE3* out, LPUNKNOWN) {
    Surface* rt = Surface::From(surf);
    if (!rt || !out) return DDERR_INVALIDPARAMS;
    if (!Renderer::Init()) return DDERR_GENERIC;
    *out = new Device(D, rt);
    return D3D_OK;
}
HRESULT D3D3::CreateVertexBuffer(LPD3DVERTEXBUFFERDESC, LPDIRECT3DVERTEXBUFFER*, DWORD, LPUNKNOWN) {
    LOG_UNIMPL("CreateVertexBuffer");
    return DDERR_UNSUPPORTED;
}
HRESULT D3D3::EnumZBufferFormats(REFCLSID, LPD3DENUMPIXELFORMATSCALLBACK cb, LPVOID ctx) {
    if (!cb) return DDERR_INVALIDPARAMS;
    for (DWORD bits : {16u, 24u, 32u}) {
        PixelFormat z;
        z.zbuffer = true;
        z.zbits = bits;
        DDPIXELFORMAT pf = z.ToDD();
        if (bits == 24) pf.dwZBitMask = 0x00FFFFFF;
        if (cb(&pf, ctx) == D3DENUMRET_CANCEL) break;
    }
    return D3D_OK;
}
HRESULT D3D3::EvictManagedTextures() { return D3D_OK; }

// ---------------------------------------------------------------------------------------------
// Viewport
// ---------------------------------------------------------------------------------------------
Viewport::Viewport() {
    impl = this;
    vp.dwSize = sizeof(vp);
    vp.dwWidth = g_mode.width;
    vp.dwHeight = g_mode.height;
    vp.dvClipX = -1.0f; vp.dvClipY = 1.0f; vp.dvClipWidth = 2.0f; vp.dvClipHeight = 2.0f;
    vp.dvMinZ = 0.0f; vp.dvMaxZ = 1.0f;
}

HRESULT Viewport::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3DViewport3 || riid == IID_IDirect3DViewport2 || riid == IID_IDirect3DViewport) {
        *ppv = this; AddRef(); return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
ULONG Viewport::AddRef() { return ++refs; }
ULONG Viewport::Release() { LONG r = --refs; if (!r) delete this; return (ULONG)r; }
HRESULT Viewport::Initialize(LPDIRECT3D) { return DDERR_ALREADYINITIALIZED; }
HRESULT Viewport::GetViewport(LPD3DVIEWPORT v) {
    if (!v) return DDERR_INVALIDPARAMS;
    memset(v, 0, sizeof(*v));
    v->dwSize = sizeof(*v);
    v->dwX = vp.dwX; v->dwY = vp.dwY; v->dwWidth = vp.dwWidth; v->dwHeight = vp.dwHeight;
    v->dvScaleX = vp.dwWidth / 2.0f; v->dvScaleY = vp.dwHeight / 2.0f;
    v->dvMaxX = 1.0f; v->dvMaxY = 1.0f; v->dvMinZ = vp.dvMinZ; v->dvMaxZ = vp.dvMaxZ;
    return D3D_OK;
}
HRESULT Viewport::SetViewport(LPD3DVIEWPORT v) {
    if (!v) return DDERR_INVALIDPARAMS;
    Renderer::Flush();
    vp.dwX = v->dwX; vp.dwY = v->dwY; vp.dwWidth = v->dwWidth; vp.dwHeight = v->dwHeight;
    vp.dvMinZ = v->dvMinZ; vp.dvMaxZ = v->dvMaxZ;
    return D3D_OK;
}
HRESULT Viewport::TransformVertices(DWORD, LPD3DTRANSFORMDATA, DWORD, LPDWORD) { LOG_UNIMPL("TransformVertices"); return DDERR_UNSUPPORTED; }
HRESULT Viewport::LightElements(DWORD, LPD3DLIGHTDATA) { return DDERR_UNSUPPORTED; }
HRESULT Viewport::SetBackground(D3DMATERIALHANDLE) { return D3D_OK; }
HRESULT Viewport::GetBackground(LPD3DMATERIALHANDLE h, LPBOOL valid) { if (h) *h = 0; if (valid) *valid = FALSE; return D3D_OK; }
HRESULT Viewport::SetBackgroundDepth(LPDIRECTDRAWSURFACE) { return DDERR_UNSUPPORTED; }
HRESULT Viewport::GetBackgroundDepth(LPDIRECTDRAWSURFACE*, LPBOOL) { return DDERR_UNSUPPORTED; }
HRESULT Viewport::Clear(DWORD count, LPD3DRECT rects, DWORD flags) {
    return Clear2(count, rects, flags, 0, 1.0f, 0);
}
HRESULT Viewport::AddLight(LPDIRECT3DLIGHT) { return DDERR_UNSUPPORTED; }
HRESULT Viewport::DeleteLight(LPDIRECT3DLIGHT) { return DDERR_UNSUPPORTED; }
HRESULT Viewport::NextLight(LPDIRECT3DLIGHT, LPDIRECT3DLIGHT*, DWORD) { return DDERR_UNSUPPORTED; }
HRESULT Viewport::GetViewport2(LPD3DVIEWPORT2 v) { if (!v) return DDERR_INVALIDPARAMS; *v = vp; return D3D_OK; }
HRESULT Viewport::SetViewport2(LPD3DVIEWPORT2 v) {
    if (!v) return DDERR_INVALIDPARAMS;
    Renderer::Flush();
    vp = *v;
    vp.dwSize = sizeof(vp);
    return D3D_OK;
}
HRESULT Viewport::SetBackgroundDepth2(LPDIRECTDRAWSURFACE4) { return DDERR_UNSUPPORTED; }
HRESULT Viewport::GetBackgroundDepth2(LPDIRECTDRAWSURFACE4*, LPBOOL) { return DDERR_UNSUPPORTED; }
HRESULT Viewport::Clear2(DWORD count, LPD3DRECT rects, DWORD flags, D3DCOLOR color, D3DVALUE z, DWORD) {
    if (!device || !device->target) return D3D_OK;
    Surface* rt = device->target;
    Surface* zs = rt->AttachedZ();
    for (DWORD i = 0; i < count; ++i) {
        RECT r{rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2};
        Renderer::ClearRect((flags & D3DCLEAR_TARGET) ? rt : nullptr, zs, r, flags, color, z);
    }
    return D3D_OK;
}
