#pragma once
// DirectDraw 4 / Direct3D 6 emulation on Direct3D 11, scoped to what OMT.dll uses.
#define DIRECTDRAW_VERSION 0x0700
#define DIRECT3D_VERSION   0x0700
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <d3d11_1.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>
#include "log.h"

#ifndef LOG_MODULE
#define LOG_MODULE "ddraw"
#endif

struct DirectDraw;
struct Surface;
struct Device;
struct Viewport;
struct Palette;

// Virtual display mode the game believes it is running in.
struct DisplayMode { int width = 640, height = 480, bpp = 16; };
extern DisplayMode g_mode;

// ---------------------------------------------------------------------------------------------
// Pixel formats
// ---------------------------------------------------------------------------------------------
struct PixelFormat {
    int bpp = 16;
    uint32_t r = 0xF800, g = 0x07E0, b = 0x001F, a = 0;  // channel masks
    bool zbuffer = false;
    uint32_t zbits = 0;
    DDPIXELFORMAT ToDD() const;
    static PixelFormat FromDD(const DDPIXELFORMAT& pf);
    static PixelFormat Rgb565() { return {}; }
    bool operator==(const PixelFormat& o) const { return bpp == o.bpp && r == o.r && g == o.g && b == o.b && a == o.a && zbuffer == o.zbuffer; }
};

// Precomputed shifts to expand a packed pixel into 8-bit channels.
struct ChannelDecoder {
    int shift[4] = {}, bits[4] = {};
    uint32_t mask[4] = {};
    explicit ChannelDecoder(const PixelFormat& pf);
    uint32_t ToBGRA(uint32_t px) const;          // returns 0xAARRGGBB
    uint32_t FromBGRA(uint32_t argb) const;
};

// ---------------------------------------------------------------------------------------------
// Surface storage. Swapped wholesale between front and back buffers on Flip.
// ---------------------------------------------------------------------------------------------
enum class Where { Cpu, Gpu, Both };

struct Storage {
    // CPU copy lives in a DIB section so GetDC works without copying.
    HBITMAP dib = nullptr;
    HDC dc = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    uint8_t* bits = nullptr;
    int pitch = 0;
    // GPU copy, created on first use.
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    Where where = Where::Cpu;
    ~Storage();
};

// Thin COM faces. Each points back to the implementing object; `magic` lets us recognise our own
// interface pointers when the game passes them back in.
template <class Impl> constexpr uint32_t FaceMagic() { return 0x46324253; }      // 'SB2F'
template <> constexpr uint32_t FaceMagic<Surface>() { return 0x46525553; }     // 'SURF'

template <class Iface, class Impl>
struct Face : Iface {
    uint32_t magic = FaceMagic<Impl>();
    Impl* impl = nullptr;
};

// ---------------------------------------------------------------------------------------------
// Surface
// ---------------------------------------------------------------------------------------------
struct SurfaceV4;
struct SurfaceV1;
struct TextureFace;

struct Surface {
    std::atomic<LONG> refs{1};
    DirectDraw* dd = nullptr;
    DDSURFACEDESC2 desc{};       // as created; lPitch/lpSurface filled on Lock
    PixelFormat format;
    int width = 0, height = 0;
    DWORD caps = 0, caps2 = 0;
    bool primary = false, backbuffer = false, zbuffer = false, texture = false, renderTarget = false;
    std::unique_ptr<Storage> store;

    bool hasSrcKey = false;
    DDCOLORKEY srcKey{};
    Palette* palette = nullptr;
    IDirectDrawClipper* clipper = nullptr;
    std::vector<Surface*> attached;  // back buffer, z-buffer, next mip level (each holds a reference)
    Surface* parentMip = nullptr;    // top of the mip chain this level belongs to
    int mipLevel = 0;
    int lockCount = 0;
    bool lockWrites = false;
    DWORD uniqueness = 1;
    bool textureDirty = true;        // texture contents changed since last GPU upload

    SurfaceV4* v4;
    SurfaceV1* v1;
    TextureFace* tex2;

    Surface(DirectDraw* owner, const DDSURFACEDESC2& d);
    ~Surface();
    ULONG AddRef() { return ++refs; }
    ULONG Release();

    static Surface* From(void* anyFace);  // accepts any of our surface interface pointers

    // Coherency between the CPU DIB and the GPU texture.
    void EnsureGpu();                    // create GPU resources if missing
    void SyncToCpu();                    // GPU -> CPU if GPU is newer
    void SyncToGpu();                    // CPU -> GPU if CPU is newer
    void MarkCpuWritten() { store->where = Where::Cpu; textureDirty = true; ++uniqueness; }
    void MarkGpuWritten() { store->where = Where::Gpu; ++uniqueness; }

    Surface* AttachedZ() const;
    Surface* TopMip() { return parentMip ? parentMip : this; }

    // DirectDraw operations shared by all interface versions.
    HRESULT Lock(RECT* rect, DDSURFACEDESC2* out, DWORD flags);
    HRESULT Unlock();
    HRESULT Blt(RECT* dst, Surface* src, RECT* srcRect, DWORD flags, DDBLTFX* fx);
    HRESULT BltFast(DWORD x, DWORD y, Surface* src, RECT* srcRect, DWORD flags);
    HRESULT Flip(Surface* target, DWORD flags);
    HRESULT GetAttached(DWORD caps, Surface** out);
    HRESULT GetDC(HDC* out);
    HRESULT ReleaseDC(HDC dc);
    void FillDesc(DDSURFACEDESC2* out) const;
};

struct SurfaceV4 : Face<IDirectDrawSurface4, Surface> {
    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    // IDirectDrawSurface4
    STDMETHOD(AddAttachedSurface)(LPDIRECTDRAWSURFACE4) override;
    STDMETHOD(AddOverlayDirtyRect)(LPRECT) override;
    STDMETHOD(Blt)(LPRECT, LPDIRECTDRAWSURFACE4, LPRECT, DWORD, LPDDBLTFX) override;
    STDMETHOD(BltBatch)(LPDDBLTBATCH, DWORD, DWORD) override;
    STDMETHOD(BltFast)(DWORD, DWORD, LPDIRECTDRAWSURFACE4, LPRECT, DWORD) override;
    STDMETHOD(DeleteAttachedSurface)(DWORD, LPDIRECTDRAWSURFACE4) override;
    STDMETHOD(EnumAttachedSurfaces)(LPVOID, LPDDENUMSURFACESCALLBACK2) override;
    STDMETHOD(EnumOverlayZOrders)(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK2) override;
    STDMETHOD(Flip)(LPDIRECTDRAWSURFACE4, DWORD) override;
    STDMETHOD(GetAttachedSurface)(LPDDSCAPS2, LPDIRECTDRAWSURFACE4*) override;
    STDMETHOD(GetBltStatus)(DWORD) override;
    STDMETHOD(GetCaps)(LPDDSCAPS2) override;
    STDMETHOD(GetClipper)(LPDIRECTDRAWCLIPPER*) override;
    STDMETHOD(GetColorKey)(DWORD, LPDDCOLORKEY) override;
    STDMETHOD(GetDC)(HDC*) override;
    STDMETHOD(GetFlipStatus)(DWORD) override;
    STDMETHOD(GetOverlayPosition)(LPLONG, LPLONG) override;
    STDMETHOD(GetPalette)(LPDIRECTDRAWPALETTE*) override;
    STDMETHOD(GetPixelFormat)(LPDDPIXELFORMAT) override;
    STDMETHOD(GetSurfaceDesc)(LPDDSURFACEDESC2) override;
    STDMETHOD(Initialize)(LPDIRECTDRAW, LPDDSURFACEDESC2) override;
    STDMETHOD(IsLost)() override;
    STDMETHOD(Lock)(LPRECT, LPDDSURFACEDESC2, DWORD, HANDLE) override;
    STDMETHOD(ReleaseDC)(HDC) override;
    STDMETHOD(Restore)() override;
    STDMETHOD(SetClipper)(LPDIRECTDRAWCLIPPER) override;
    STDMETHOD(SetColorKey)(DWORD, LPDDCOLORKEY) override;
    STDMETHOD(SetOverlayPosition)(LONG, LONG) override;
    STDMETHOD(SetPalette)(LPDIRECTDRAWPALETTE) override;
    STDMETHOD(Unlock)(LPRECT) override;
    STDMETHOD(UpdateOverlay)(LPRECT, LPDIRECTDRAWSURFACE4, LPRECT, DWORD, LPDDOVERLAYFX) override;
    STDMETHOD(UpdateOverlayDisplay)(DWORD) override;
    STDMETHOD(UpdateOverlayZOrder)(DWORD, LPDIRECTDRAWSURFACE4) override;
    STDMETHOD(GetDDInterface)(LPVOID*) override;
    STDMETHOD(PageLock)(DWORD) override;
    STDMETHOD(PageUnlock)(DWORD) override;
    STDMETHOD(SetSurfaceDesc)(LPDDSURFACEDESC2, DWORD) override;
    STDMETHOD(SetPrivateData)(REFGUID, LPVOID, DWORD, DWORD) override;
    STDMETHOD(GetPrivateData)(REFGUID, LPVOID, LPDWORD) override;
    STDMETHOD(FreePrivateData)(REFGUID) override;
    STDMETHOD(GetUniquenessValue)(LPDWORD) override;
    STDMETHOD(ChangeUniquenessValue)() override;
};

// Version 1 face, for code that queries IDirectDrawSurface (e.g. RAD's Bink).
struct SurfaceV1 : Face<IDirectDrawSurface, Surface> {
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(AddAttachedSurface)(LPDIRECTDRAWSURFACE) override;
    STDMETHOD(AddOverlayDirtyRect)(LPRECT) override;
    STDMETHOD(Blt)(LPRECT, LPDIRECTDRAWSURFACE, LPRECT, DWORD, LPDDBLTFX) override;
    STDMETHOD(BltBatch)(LPDDBLTBATCH, DWORD, DWORD) override;
    STDMETHOD(BltFast)(DWORD, DWORD, LPDIRECTDRAWSURFACE, LPRECT, DWORD) override;
    STDMETHOD(DeleteAttachedSurface)(DWORD, LPDIRECTDRAWSURFACE) override;
    STDMETHOD(EnumAttachedSurfaces)(LPVOID, LPDDENUMSURFACESCALLBACK) override;
    STDMETHOD(EnumOverlayZOrders)(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK) override;
    STDMETHOD(Flip)(LPDIRECTDRAWSURFACE, DWORD) override;
    STDMETHOD(GetAttachedSurface)(LPDDSCAPS, LPDIRECTDRAWSURFACE*) override;
    STDMETHOD(GetBltStatus)(DWORD) override;
    STDMETHOD(GetCaps)(LPDDSCAPS) override;
    STDMETHOD(GetClipper)(LPDIRECTDRAWCLIPPER*) override;
    STDMETHOD(GetColorKey)(DWORD, LPDDCOLORKEY) override;
    STDMETHOD(GetDC)(HDC*) override;
    STDMETHOD(GetFlipStatus)(DWORD) override;
    STDMETHOD(GetOverlayPosition)(LPLONG, LPLONG) override;
    STDMETHOD(GetPalette)(LPDIRECTDRAWPALETTE*) override;
    STDMETHOD(GetPixelFormat)(LPDDPIXELFORMAT) override;
    STDMETHOD(GetSurfaceDesc)(LPDDSURFACEDESC) override;
    STDMETHOD(Initialize)(LPDIRECTDRAW, LPDDSURFACEDESC) override;
    STDMETHOD(IsLost)() override;
    STDMETHOD(Lock)(LPRECT, LPDDSURFACEDESC, DWORD, HANDLE) override;
    STDMETHOD(ReleaseDC)(HDC) override;
    STDMETHOD(Restore)() override;
    STDMETHOD(SetClipper)(LPDIRECTDRAWCLIPPER) override;
    STDMETHOD(SetColorKey)(DWORD, LPDDCOLORKEY) override;
    STDMETHOD(SetOverlayPosition)(LONG, LONG) override;
    STDMETHOD(SetPalette)(LPDIRECTDRAWPALETTE) override;
    STDMETHOD(Unlock)(LPVOID) override;
    STDMETHOD(UpdateOverlay)(LPRECT, LPDIRECTDRAWSURFACE, LPRECT, DWORD, LPDDOVERLAYFX) override;
    STDMETHOD(UpdateOverlayDisplay)(DWORD) override;
    STDMETHOD(UpdateOverlayZOrder)(DWORD, LPDIRECTDRAWSURFACE) override;
};

struct TextureFace : Face<IDirect3DTexture2, Surface> {
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(GetHandle)(LPDIRECT3DDEVICE2, LPD3DTEXTUREHANDLE) override;
    STDMETHOD(PaletteChanged)(DWORD, DWORD) override;
    STDMETHOD(Load)(LPDIRECT3DTEXTURE2) override;
};

// ---------------------------------------------------------------------------------------------
// Palette (8-bit modes are not used by the game at 16 bpp, but the engine may create one).
// ---------------------------------------------------------------------------------------------
struct Palette : Face<IDirectDrawPalette, Palette> {
    std::atomic<LONG> refs{1};
    PALETTEENTRY entries[256]{};
    DWORD flags = 0;
    Palette() { impl = this; }
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(GetCaps)(LPDWORD) override;
    STDMETHOD(GetEntries)(DWORD, DWORD, DWORD, LPPALETTEENTRY) override;
    STDMETHOD(Initialize)(LPDIRECTDRAW, DWORD, LPPALETTEENTRY) override;
    STDMETHOD(SetEntries)(DWORD, DWORD, DWORD, LPPALETTEENTRY) override;
};

struct Clipper : Face<IDirectDrawClipper, Clipper> {
    std::atomic<LONG> refs{1};
    HWND hwnd = nullptr;
    Clipper() { impl = this; }
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(GetClipList)(LPRECT, LPRGNDATA, LPDWORD) override;
    STDMETHOD(GetHWnd)(HWND*) override;
    STDMETHOD(Initialize)(LPDIRECTDRAW, DWORD) override;
    STDMETHOD(IsClipListChanged)(BOOL*) override;
    STDMETHOD(SetClipList)(LPRGNDATA, DWORD) override;
    STDMETHOD(SetHWnd)(DWORD, HWND) override;
};

// ---------------------------------------------------------------------------------------------
// DirectDraw object: IDirectDraw, IDirectDraw4 and IDirect3D3 faces share one reference count.
// ---------------------------------------------------------------------------------------------
struct DDv1;
struct DDv4;
struct D3D3;

struct DirectDraw {
    std::atomic<LONG> refs{1};
    HWND hwnd = nullptr;
    DWORD coopFlags = 0;
    DDv1* v1;
    DDv4* v4;
    D3D3* d3d;

    DirectDraw();
    ~DirectDraw();
    ULONG AddRef() { return ++refs; }
    ULONG Release();
    HRESULT QueryInterface(REFIID riid, void** ppv);

    HRESULT SetCooperativeLevel(HWND w, DWORD flags);
    HRESULT CreateSurface(const DDSURFACEDESC2& desc, Surface** out);
    HRESULT EnumDisplayModes(DWORD flags, LPDDSURFACEDESC2 filter, void* ctx, LPDDENUMMODESCALLBACK2 cb2, LPDDENUMMODESCALLBACK cb1);
    void GetCaps(DDCAPS* hw, DDCAPS* hel);
    void GetDisplayMode(DDSURFACEDESC2* d);
};

struct DDv1 : Face<IDirectDraw, DirectDraw> {
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(Compact)() override;
    STDMETHOD(CreateClipper)(DWORD, LPDIRECTDRAWCLIPPER*, IUnknown*) override;
    STDMETHOD(CreatePalette)(DWORD, LPPALETTEENTRY, LPDIRECTDRAWPALETTE*, IUnknown*) override;
    STDMETHOD(CreateSurface)(LPDDSURFACEDESC, LPDIRECTDRAWSURFACE*, IUnknown*) override;
    STDMETHOD(DuplicateSurface)(LPDIRECTDRAWSURFACE, LPDIRECTDRAWSURFACE*) override;
    STDMETHOD(EnumDisplayModes)(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMMODESCALLBACK) override;
    STDMETHOD(EnumSurfaces)(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMSURFACESCALLBACK) override;
    STDMETHOD(FlipToGDISurface)() override;
    STDMETHOD(GetCaps)(LPDDCAPS, LPDDCAPS) override;
    STDMETHOD(GetDisplayMode)(LPDDSURFACEDESC) override;
    STDMETHOD(GetFourCCCodes)(LPDWORD, LPDWORD) override;
    STDMETHOD(GetGDISurface)(LPDIRECTDRAWSURFACE*) override;
    STDMETHOD(GetMonitorFrequency)(LPDWORD) override;
    STDMETHOD(GetScanLine)(LPDWORD) override;
    STDMETHOD(GetVerticalBlankStatus)(LPBOOL) override;
    STDMETHOD(Initialize)(GUID*) override;
    STDMETHOD(RestoreDisplayMode)() override;
    STDMETHOD(SetCooperativeLevel)(HWND, DWORD) override;
    STDMETHOD(SetDisplayMode)(DWORD, DWORD, DWORD) override;
    STDMETHOD(WaitForVerticalBlank)(DWORD, HANDLE) override;
};

struct DDv4 : Face<IDirectDraw4, DirectDraw> {
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(Compact)() override;
    STDMETHOD(CreateClipper)(DWORD, LPDIRECTDRAWCLIPPER*, IUnknown*) override;
    STDMETHOD(CreatePalette)(DWORD, LPPALETTEENTRY, LPDIRECTDRAWPALETTE*, IUnknown*) override;
    STDMETHOD(CreateSurface)(LPDDSURFACEDESC2, LPDIRECTDRAWSURFACE4*, IUnknown*) override;
    STDMETHOD(DuplicateSurface)(LPDIRECTDRAWSURFACE4, LPDIRECTDRAWSURFACE4*) override;
    STDMETHOD(EnumDisplayModes)(DWORD, LPDDSURFACEDESC2, LPVOID, LPDDENUMMODESCALLBACK2) override;
    STDMETHOD(EnumSurfaces)(DWORD, LPDDSURFACEDESC2, LPVOID, LPDDENUMSURFACESCALLBACK2) override;
    STDMETHOD(FlipToGDISurface)() override;
    STDMETHOD(GetCaps)(LPDDCAPS, LPDDCAPS) override;
    STDMETHOD(GetDisplayMode)(LPDDSURFACEDESC2) override;
    STDMETHOD(GetFourCCCodes)(LPDWORD, LPDWORD) override;
    STDMETHOD(GetGDISurface)(LPDIRECTDRAWSURFACE4*) override;
    STDMETHOD(GetMonitorFrequency)(LPDWORD) override;
    STDMETHOD(GetScanLine)(LPDWORD) override;
    STDMETHOD(GetVerticalBlankStatus)(LPBOOL) override;
    STDMETHOD(Initialize)(GUID*) override;
    STDMETHOD(RestoreDisplayMode)() override;
    STDMETHOD(SetCooperativeLevel)(HWND, DWORD) override;
    STDMETHOD(SetDisplayMode)(DWORD, DWORD, DWORD, DWORD, DWORD) override;
    STDMETHOD(WaitForVerticalBlank)(DWORD, HANDLE) override;
    STDMETHOD(GetAvailableVidMem)(LPDDSCAPS2, LPDWORD, LPDWORD) override;
    STDMETHOD(GetSurfaceFromDC)(HDC, LPDIRECTDRAWSURFACE4*) override;
    STDMETHOD(RestoreAllSurfaces)() override;
    STDMETHOD(TestCooperativeLevel)() override;
    STDMETHOD(GetDeviceIdentifier)(LPDDDEVICEIDENTIFIER, DWORD) override;
};

struct D3D3 : Face<IDirect3D3, DirectDraw> {
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(EnumDevices)(LPD3DENUMDEVICESCALLBACK, LPVOID) override;
    STDMETHOD(CreateLight)(LPDIRECT3DLIGHT*, LPUNKNOWN) override;
    STDMETHOD(CreateMaterial)(LPDIRECT3DMATERIAL3*, LPUNKNOWN) override;
    STDMETHOD(CreateViewport)(LPDIRECT3DVIEWPORT3*, LPUNKNOWN) override;
    STDMETHOD(FindDevice)(LPD3DFINDDEVICESEARCH, LPD3DFINDDEVICERESULT) override;
    STDMETHOD(CreateDevice)(REFCLSID, LPDIRECTDRAWSURFACE4, LPDIRECT3DDEVICE3*, LPUNKNOWN) override;
    STDMETHOD(CreateVertexBuffer)(LPD3DVERTEXBUFFERDESC, LPDIRECT3DVERTEXBUFFER*, DWORD, LPUNKNOWN) override;
    STDMETHOD(EnumZBufferFormats)(REFCLSID, LPD3DENUMPIXELFORMATSCALLBACK, LPVOID) override;
    STDMETHOD(EvictManagedTextures)() override;
};

void FillDeviceDesc(D3DDEVICEDESC* d);

// ---------------------------------------------------------------------------------------------
// Viewport and device
// ---------------------------------------------------------------------------------------------
struct Viewport : Face<IDirect3DViewport3, Viewport> {
    std::atomic<LONG> refs{1};
    D3DVIEWPORT2 vp{};
    Device* device = nullptr;
    Viewport();
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(Initialize)(LPDIRECT3D) override;
    STDMETHOD(GetViewport)(LPD3DVIEWPORT) override;
    STDMETHOD(SetViewport)(LPD3DVIEWPORT) override;
    STDMETHOD(TransformVertices)(DWORD, LPD3DTRANSFORMDATA, DWORD, LPDWORD) override;
    STDMETHOD(LightElements)(DWORD, LPD3DLIGHTDATA) override;
    STDMETHOD(SetBackground)(D3DMATERIALHANDLE) override;
    STDMETHOD(GetBackground)(LPD3DMATERIALHANDLE, LPBOOL) override;
    STDMETHOD(SetBackgroundDepth)(LPDIRECTDRAWSURFACE) override;
    STDMETHOD(GetBackgroundDepth)(LPDIRECTDRAWSURFACE*, LPBOOL) override;
    STDMETHOD(Clear)(DWORD, LPD3DRECT, DWORD) override;
    STDMETHOD(AddLight)(LPDIRECT3DLIGHT) override;
    STDMETHOD(DeleteLight)(LPDIRECT3DLIGHT) override;
    STDMETHOD(NextLight)(LPDIRECT3DLIGHT, LPDIRECT3DLIGHT*, DWORD) override;
    STDMETHOD(GetViewport2)(LPD3DVIEWPORT2) override;
    STDMETHOD(SetViewport2)(LPD3DVIEWPORT2) override;
    STDMETHOD(SetBackgroundDepth2)(LPDIRECTDRAWSURFACE4) override;
    STDMETHOD(GetBackgroundDepth2)(LPDIRECTDRAWSURFACE4*, LPBOOL) override;
    STDMETHOD(Clear2)(DWORD, LPD3DRECT, DWORD, D3DCOLOR, D3DVALUE, DWORD) override;
};

struct Device : Face<IDirect3DDevice3, Device> {
    std::atomic<LONG> refs{1};
    DirectDraw* dd = nullptr;
    Surface* target = nullptr;
    Viewport* viewport = nullptr;
    std::vector<Viewport*> viewports;
    DWORD rs[256] = {};
    DWORD tss[8][32] = {};
    Surface* textures[8] = {};
    bool inScene = false;

    Device(DirectDraw* owner, Surface* rt);
    ~Device();
    void SetDefaults();
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(GetCaps)(LPD3DDEVICEDESC, LPD3DDEVICEDESC) override;
    STDMETHOD(GetStats)(LPD3DSTATS) override;
    STDMETHOD(AddViewport)(LPDIRECT3DVIEWPORT3) override;
    STDMETHOD(DeleteViewport)(LPDIRECT3DVIEWPORT3) override;
    STDMETHOD(NextViewport)(LPDIRECT3DVIEWPORT3, LPDIRECT3DVIEWPORT3*, DWORD) override;
    STDMETHOD(EnumTextureFormats)(LPD3DENUMPIXELFORMATSCALLBACK, LPVOID) override;
    STDMETHOD(BeginScene)() override;
    STDMETHOD(EndScene)() override;
    STDMETHOD(GetDirect3D)(LPDIRECT3D3*) override;
    STDMETHOD(SetCurrentViewport)(LPDIRECT3DVIEWPORT3) override;
    STDMETHOD(GetCurrentViewport)(LPDIRECT3DVIEWPORT3*) override;
    STDMETHOD(SetRenderTarget)(LPDIRECTDRAWSURFACE4, DWORD) override;
    STDMETHOD(GetRenderTarget)(LPDIRECTDRAWSURFACE4*) override;
    STDMETHOD(Begin)(D3DPRIMITIVETYPE, DWORD, DWORD) override;
    STDMETHOD(BeginIndexed)(D3DPRIMITIVETYPE, DWORD, LPVOID, DWORD, DWORD) override;
    STDMETHOD(Vertex)(LPVOID) override;
    STDMETHOD(Index)(WORD) override;
    STDMETHOD(End)(DWORD) override;
    STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE, LPDWORD) override;
    STDMETHOD(SetRenderState)(D3DRENDERSTATETYPE, DWORD) override;
    STDMETHOD(GetLightState)(D3DLIGHTSTATETYPE, LPDWORD) override;
    STDMETHOD(SetLightState)(D3DLIGHTSTATETYPE, DWORD) override;
    STDMETHOD(SetTransform)(D3DTRANSFORMSTATETYPE, LPD3DMATRIX) override;
    STDMETHOD(GetTransform)(D3DTRANSFORMSTATETYPE, LPD3DMATRIX) override;
    STDMETHOD(MultiplyTransform)(D3DTRANSFORMSTATETYPE, LPD3DMATRIX) override;
    STDMETHOD(DrawPrimitive)(D3DPRIMITIVETYPE, DWORD, LPVOID, DWORD, DWORD) override;
    STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE, DWORD, LPVOID, DWORD, LPWORD, DWORD, DWORD) override;
    STDMETHOD(SetClipStatus)(LPD3DCLIPSTATUS) override;
    STDMETHOD(GetClipStatus)(LPD3DCLIPSTATUS) override;
    STDMETHOD(DrawPrimitiveStrided)(D3DPRIMITIVETYPE, DWORD, LPD3DDRAWPRIMITIVESTRIDEDDATA, DWORD, DWORD) override;
    STDMETHOD(DrawIndexedPrimitiveStrided)(D3DPRIMITIVETYPE, DWORD, LPD3DDRAWPRIMITIVESTRIDEDDATA, DWORD, LPWORD, DWORD, DWORD) override;
    STDMETHOD(DrawPrimitiveVB)(D3DPRIMITIVETYPE, LPDIRECT3DVERTEXBUFFER, DWORD, DWORD, DWORD) override;
    STDMETHOD(DrawIndexedPrimitiveVB)(D3DPRIMITIVETYPE, LPDIRECT3DVERTEXBUFFER, LPWORD, DWORD, DWORD) override;
    STDMETHOD(ComputeSphereVisibility)(LPD3DVECTOR, LPD3DVALUE, DWORD, DWORD, LPDWORD) override;
    STDMETHOD(GetTexture)(DWORD, LPDIRECT3DTEXTURE2*) override;
    STDMETHOD(SetTexture)(DWORD, LPDIRECT3DTEXTURE2) override;
    STDMETHOD(GetTextureStageState)(DWORD, D3DTEXTURESTAGESTATETYPE, LPDWORD) override;
    STDMETHOD(SetTextureStageState)(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD) override;
    STDMETHOD(ValidateDevice)(LPDWORD) override;

    HRESULT Draw(D3DPRIMITIVETYPE type, DWORD fvf, const uint8_t* verts, DWORD count, const WORD* indices, DWORD indexCount);
};

// ---------------------------------------------------------------------------------------------
// Renderer: owns the D3D11 device, the game window and presentation.
// ---------------------------------------------------------------------------------------------
struct GpuVertex {
    float pos[4];
    float diffuse[4];
    float specular[4];
    float uv0[2];
    float uv1[2];
};

namespace Renderer {
    bool Init();
    ID3D11Device* Dev();
    ID3D11DeviceContext1* Ctx();

    void AttachWindow(HWND hwnd);
    HWND Window();
    void Present(Surface* front);       // called on Flip, or for a non-flipping primary
    void MarkPrimaryDirty(Surface* primary);
    void PresentIfDirty();              // frame boundary for non-flipping primaries
    uint64_t PresentCount();

    // Maps a client-area point to virtual 640x480 coordinates and back.
    POINT ClientToGame(POINT client);
    POINT GameToClient(POINT game);
    bool IsInsideGameImage(POINT client);

    // GPU texture management for surfaces.
    void CreateSurfaceResources(Surface* s);
    void Upload(Surface* s);
    void Readback(Surface* s);

    // 3D batching.
    void BeginDraw(Device* dev);        // binds render target / depth of the device
    void SubmitTriangles(Device* dev, const GpuVertex* v, size_t count, D3D11_PRIMITIVE_TOPOLOGY topo);
    void Flush();                       // issues any pending batched geometry
    void ClearRect(Surface* rt, Surface* z, const RECT& r, DWORD flags, D3DCOLOR color, float zval);
}

// Compatibility hooks installed when the DLL loads.
namespace Compat {
    void Install();
    void OnFramePresented();
}
