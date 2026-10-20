#include "ddraw_impl.h"
#include "config.h"
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>

namespace {

// ---------------------------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------------------------
const char* kGameShader = R"(
cbuffer VSConst : register(b0) { float4 rtInfo; };  // x = 2/width, y = 2/height

struct VSIn {
    float4 pos : POSITION;
    float4 diff : COLOR0;
    float4 spec : COLOR1;
    float2 t0 : TEXCOORD0;
    float2 t1 : TEXCOORD1;
};
struct PSIn {
    float4 pos : SV_Position;
    float4 diff : COLOR0;
    float4 spec : COLOR1;
    nointerpolation float4 diffFlat : COLOR2;
    nointerpolation float4 specFlat : COLOR3;
    float2 t0 : TEXCOORD0;
    float2 t1 : TEXCOORD1;
};

PSIn VSMain(VSIn i) {
    PSIn o;
    float w = i.pos.w > 1e-8 ? 1.0 / i.pos.w : 1.0;
    // Pre-transformed D3D6 vertices use integer pixel centres; D3D11 samples at +0.5.
    float x = (i.pos.x + 0.5) * rtInfo.x - 1.0;
    float y = 1.0 - (i.pos.y + 0.5) * rtInfo.y;
    o.pos = float4(x * w, y * w, saturate(i.pos.z) * w, w);
    o.diff = i.diff; o.diffFlat = i.diff;
    o.spec = i.spec; o.specFlat = i.spec;
    o.t0 = i.t0; o.t1 = i.t1;
    return o;
}

Texture2D tex0 : register(t0);
Texture2D tex1 : register(t1);
SamplerState samp0 : register(s0);
SamplerState samp1 : register(s1);

cbuffer PSConst : register(b1) {
    uint4 s0a;    // colorOp, colorArg1, colorArg2, alphaOp
    uint4 s0b;    // alphaArg1, alphaArg2, hasTexture, textureHasAlpha
    uint4 s1a;
    uint4 s1b;
    uint4 misc;   // x: 1 colorkey, 2 alpha test, 4 fog, 8 specular, 16 flat shading; y: alpha func
    float4 tfactor;
    float4 fogColor;
    float4 alphaRef;
};

float4 Arg(uint a, float4 diffuse, float4 current, float4 texel, float4 specular) {
    uint sel = a & 0xF;
    float4 r = sel == 0 ? diffuse : sel == 1 ? current : sel == 2 ? texel : sel == 3 ? tfactor : specular;
    if (a & 0x10) r = 1.0 - r;
    if (a & 0x20) r = r.aaaa;
    return r;
}

float4 Combine(uint op, float4 a1, float4 a2, float4 diffuse, float4 texel, float4 current) {
    switch (op) {
    case 2: return a1;
    case 3: return a2;
    case 4: return a1 * a2;
    case 5: return saturate(a1 * a2 * 2.0);
    case 6: return saturate(a1 * a2 * 4.0);
    case 7: return saturate(a1 + a2);
    case 8: return saturate(a1 + a2 - 0.5);
    case 9: return saturate((a1 + a2 - 0.5) * 2.0);
    case 10: return saturate(a1 - a2);
    case 11: return saturate(a1 + a2 - a1 * a2);
    case 12: return lerp(a2, a1, diffuse.a);
    case 13: return lerp(a2, a1, texel.a);
    case 14: return lerp(a2, a1, tfactor.a);
    case 15: return saturate(a1 + a2 * (1.0 - texel.a));
    case 16: return lerp(a2, a1, current.a);
    default: return a1 * a2;
    }
}

float4 Stage(uint4 a, uint4 b, float4 texel, float4 diffuse, float4 current, float4 specular) {
    float4 c1 = Arg(a.y, diffuse, current, texel, specular);
    float4 c2 = Arg(a.z, diffuse, current, texel, specular);
    float3 rgb = Combine(a.x, c1, c2, diffuse, texel, current).rgb;
    float alpha;
    if (a.w == 1) alpha = current.a;
    else if (a.w == 100) alpha = b.w != 0 ? texel.a : diffuse.a;
    else {
        float4 x1 = Arg(b.x, diffuse, current, texel, specular);
        float4 x2 = Arg(b.y, diffuse, current, texel, specular);
        alpha = Combine(a.w, x1, x2, diffuse, texel, current).a;
    }
    return float4(rgb, alpha);
}

float4 PSMain(PSIn i) : SV_Target {
    bool flat = (misc.x & 16) != 0;
    float4 diffuse = flat ? i.diffFlat : i.diff;
    float4 specular = flat ? i.specFlat : i.spec;
    float4 current = diffuse;

    if (s0a.x != 1) {
        float4 texel = s0b.z != 0 ? tex0.Sample(samp0, i.t0) : float4(1, 1, 1, 1);
        if ((misc.x & 1) && s0b.z != 0 && texel.a < 0.5) discard;
        current = Stage(s0a, s0b, texel, diffuse, current, specular);
        if (s1a.x != 1) {
            float4 texel1 = s1b.z != 0 ? tex1.Sample(samp1, i.t1) : float4(1, 1, 1, 1);
            current = Stage(s1a, s1b, texel1, diffuse, current, specular);
        }
    }
    if (misc.x & 8) current.rgb = saturate(current.rgb + specular.rgb);
    if (misc.x & 4) current.rgb = lerp(fogColor.rgb, current.rgb, specular.a);
    if (misc.x & 2) {
        float a = current.a, r = alphaRef.x;
        uint f = misc.y;
        bool ok = f == 1 ? false : f == 2 ? a < r : f == 3 ? abs(a - r) < 0.002 : f == 4 ? a <= r :
                    f == 5 ? a > r : f == 6 ? abs(a - r) >= 0.002 : f == 7 ? a >= r : true;
        if (!ok) discard;
    }
    return current;
}

// Writes depth only, for partial z-buffer clears.
cbuffer ClearConst : register(b0) { float4 clearDepth; };
float4 VSClear(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), clearDepth.x, 1);
}
)";

const char* kPresentShader = R"(
cbuffer PresentConst : register(b0) { float4 srcSize; float4 params; };  // params: x mode, yz scale
Texture2D src : register(t0);
SamplerState samp : register(s0);

struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VOut VSMain(uint id : SV_VertexID) {
    VOut o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.uv = p;
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 PSMain(VOut i) : SV_Target {
    float2 uv = i.uv;
    if (params.x == 2) {
        // Sharp bilinear: nearest-neighbour inside texels, linear only across texel edges.
        float2 texel = uv * srcSize.xy;
        float2 scale = max(params.yz, 1.0);
        float2 region = 0.5 - 0.5 / scale;
        float2 d = frac(texel) - 0.5;
        float2 f = (d - clamp(d, -region, region)) * scale + 0.5;
        uv = (floor(texel) + f) * srcSize.zw;
    }
    return float4(src.Sample(samp, uv).rgb, 1);
}
)";

struct VSConst { float rtInfo[4]; };
struct PSConst {
    uint32_t s0a[4], s0b[4], s1a[4], s1b[4], misc[4];
    float tfactor[4], fogColor[4], alphaRef[4];
};
struct PresentConst { float srcSize[4]; float params[4]; };

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------
bool g_initTried = false, g_ok = false;
ID3D11Device* g_dev;
ID3D11DeviceContext1* g_ctx;
IDXGIFactory2* g_factory;
IDXGISwapChain1* g_swap;
ID3D11RenderTargetView* g_backRtv;
bool g_tearing = false;

ID3D11VertexShader* g_vs;
ID3D11PixelShader* g_ps;
ID3D11InputLayout* g_layout;
ID3D11VertexShader* g_clearVs;
ID3D11VertexShader* g_presentVs;
ID3D11PixelShader* g_presentPs;
ID3D11Buffer* g_vb;
ID3D11Buffer* g_vsCb;
ID3D11Buffer* g_psCb;
ID3D11Buffer* g_presentCb;
ID3D11SamplerState* g_presentPoint;
ID3D11SamplerState* g_presentLinear;
const UINT kVbVertices = 1 << 16;
UINT g_vbPos = 0;

std::map<uint64_t, ID3D11SamplerState*> g_samplers;
std::map<uint64_t, ID3D11BlendState*> g_blends;
std::map<uint64_t, ID3D11DepthStencilState*> g_depths;
std::map<uint64_t, ID3D11RasterizerState*> g_rasters;
std::map<std::pair<int, int>, ID3D11Texture2D*> g_staging;

// Pending batch.
Device* g_batchDev = nullptr;
D3D11_PRIMITIVE_TOPOLOGY g_batchTopo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
std::vector<GpuVertex> g_batch;

// Window and presentation.
HWND g_hwnd;
WNDPROC g_gameProc;
bool g_windowed = false;
bool g_adjusting = false;
RECT g_dest{0, 0, 640, 480};  // where the game image lands in the client area
int g_clientW = 640, g_clientH = 480;
Surface* g_dirtyPrimary = nullptr;
uint64_t g_presentCount = 0;
LARGE_INTEGER g_qpcFreq, g_lastFrame;
HANDLE g_frameTimer;

bool Compile(const char* src, const char* entry, const char* profile, ID3DBlob** out) {
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, profile,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
    if (FAILED(hr)) {
        LogWrite(1, "render", "shader %s failed: %s", entry, err ? (const char*)err->GetBufferPointer() : "?");
        SafeRelease(err);
        return false;
    }
    SafeRelease(err);
    return true;
}

ID3D11Buffer* MakeBuffer(UINT size, UINT bind, D3D11_USAGE usage = D3D11_USAGE_DYNAMIC) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = (size + 15) & ~15u;
    bd.BindFlags = bind;
    bd.Usage = usage;
    bd.CPUAccessFlags = usage == D3D11_USAGE_DYNAMIC ? D3D11_CPU_ACCESS_WRITE : 0;
    ID3D11Buffer* b = nullptr;
    g_dev->CreateBuffer(&bd, nullptr, &b);
    return b;
}

template <class T> void WriteCb(ID3D11Buffer* b, const T& data) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &data, sizeof(T));
        g_ctx->Unmap(b, 0);
    }
}

// ---------------------------------------------------------------------------------------------
// Window layout
// ---------------------------------------------------------------------------------------------
void ComputeDest() {
    const Config& c = GetConfig();
    const int sw = g_mode.width, sh = g_mode.height;
    int w = g_clientW, h = g_clientH;
    if (c.scaling == 1) { g_dest = {0, 0, w, h}; return; }
    double scale = std::min(w / (double)sw, h / (double)sh);
    if (c.scaling == 2 && scale >= 1.0) scale = std::floor(scale);
    int dw = (int)std::lround(sw * scale), dh = (int)std::lround(sh * scale);
    g_dest.left = (w - dw) / 2;
    g_dest.top = (h - dh) / 2;
    g_dest.right = g_dest.left + dw;
    g_dest.bottom = g_dest.top + dh;
}

void ResizeSwapchain() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_clientW = std::max<int>(1, rc.right - rc.left);
    g_clientH = std::max<int>(1, rc.bottom - rc.top);
    ComputeDest();
    if (!g_swap) return;
    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    SafeRelease(g_backRtv);
    g_swap->ResizeBuffers(0, g_clientW, g_clientH, DXGI_FORMAT_UNKNOWN, g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) { g_dev->CreateRenderTargetView(back, nullptr, &g_backRtv); back->Release(); }
}

void ApplyWindowMode() {
    g_adjusting = true;
    HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfo(mon, &mi);
    LONG ex = GetWindowLong(g_hwnd, GWL_EXSTYLE) & ~(WS_EX_TOPMOST | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE);
    if (g_windowed) {
        LONG style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        SetWindowLong(g_hwnd, GWL_STYLE, style);
        SetWindowLong(g_hwnd, GWL_EXSTYLE, ex);
        int scale = GetConfig().windowScale;
        RECT r{0, 0, g_mode.width * scale, g_mode.height * scale};
        AdjustWindowRectEx(&r, style, FALSE, ex);
        int w = r.right - r.left, h = r.bottom - r.top;
        const RECT& wa = mi.rcWork;
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2,
                     w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLong(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLong(g_hwnd, GWL_EXSTYLE, ex);
        const RECT& m = mi.rcMonitor;
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    g_adjusting = false;
    ResizeSwapchain();
}

LPARAM MapMouse(LPARAM lp) {
    POINT p = Renderer::ClientToGame({(short)LOWORD(lp), (short)HIWORD(lp)});
    return MAKELPARAM(p.x, p.y);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        if (g_swap && wp != SIZE_MINIMIZED) ResizeSwapchain();
        return 0;  // the game expects a fixed 640x480 window
    case WM_MOVE:
    case WM_WINDOWPOSCHANGING:
    case WM_WINDOWPOSCHANGED:
    case WM_GETMINMAXINFO:
    case WM_NCCALCSIZE:
        return DefWindowProc(hwnd, msg, wp, lp);
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) {
            g_windowed = !g_windowed;
            ApplyWindowMode();
            return 0;
        }
        break;
    case WM_SYSCHAR:
        if (wp == VK_RETURN) return 0;  // no beep after Alt+Enter
        break;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        lp = MapMouse(lp);
        break;
    case WM_SETCURSOR:
        // Hide the Windows cursor over the game image; the game draws its own.
        if (LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            if (PtInRect(&g_dest, p)) { SetCursor(nullptr); return TRUE; }
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            return TRUE;
        }
        break;
    default:
        break;
    }
    return CallWindowProc(g_gameProc, hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------------------------
// Pixel conversion
// ---------------------------------------------------------------------------------------------
void ToBgra(const Surface* s, const Storage* st, uint32_t* dst, const DDCOLORKEY* key) {
    const int w = s->width, h = s->height;
    const PixelFormat& f = s->format;
    const bool is565 = f.bpp == 16 && f.r == 0xF800 && f.g == 0x07E0 && f.b == 0x001F && !f.a;
    if (is565 && !key) {
        for (int y = 0; y < h; ++y) {
            const uint16_t* row = (const uint16_t*)(st->bits + y * st->pitch);
            uint32_t* out = dst + y * w;
            for (int x = 0; x < w; ++x) {
                uint32_t p = row[x];
                uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
                out[x] = 0xFF000000 | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
            }
        }
        return;
    }
    ChannelDecoder dec(f);
    const int bytes = f.bpp / 8;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = st->bits + y * st->pitch;
        uint32_t* out = dst + y * w;
        for (int x = 0; x < w; ++x) {
            uint32_t px;
            switch (bytes) {
            case 1: px = row[x]; break;
            case 2: px = ((const uint16_t*)row)[x]; break;
            default: px = ((const uint32_t*)row)[x]; break;
            }
            uint32_t c = dec.ToBGRA(px);
            if (key && px >= key->dwColorSpaceLowValue && px <= key->dwColorSpaceHighValue) c &= 0x00FFFFFF;
            out[x] = c;
        }
    }
}

void FromBgra(Surface* s, const uint8_t* src, UINT srcPitch) {
    const int w = s->width, h = s->height;
    const PixelFormat& f = s->format;
    Storage* st = s->store.get();
    const bool is565 = f.bpp == 16 && f.r == 0xF800 && f.g == 0x07E0 && f.b == 0x001F;
    if (is565) {
        for (int y = 0; y < h; ++y) {
            const uint32_t* in = (const uint32_t*)(src + y * srcPitch);
            uint16_t* out = (uint16_t*)(st->bits + y * st->pitch);
            for (int x = 0; x < w; ++x) {
                uint32_t c = in[x];
                out[x] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
            }
        }
        return;
    }
    ChannelDecoder dec(f);
    const int bytes = f.bpp / 8;
    for (int y = 0; y < h; ++y) {
        const uint32_t* in = (const uint32_t*)(src + y * srcPitch);
        uint8_t* out = st->bits + y * st->pitch;
        for (int x = 0; x < w; ++x) {
            uint32_t px = dec.FromBGRA(in[x]);
            if (bytes == 2) ((uint16_t*)out)[x] = (uint16_t)px;
            else if (bytes == 4) ((uint32_t*)out)[x] = px;
            else out[x] = (uint8_t)px;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Pipeline state from D3D6 render states
// ---------------------------------------------------------------------------------------------
D3D11_BLEND MapBlend(DWORD b) {
    switch (b) {
    case D3DBLEND_ZERO: return D3D11_BLEND_ZERO;
    case D3DBLEND_ONE: return D3D11_BLEND_ONE;
    case D3DBLEND_SRCCOLOR: return D3D11_BLEND_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR: return D3D11_BLEND_INV_SRC_COLOR;
    case D3DBLEND_SRCALPHA: return D3D11_BLEND_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA: return D3D11_BLEND_INV_SRC_ALPHA;
    case D3DBLEND_DESTALPHA: return D3D11_BLEND_DEST_ALPHA;
    case D3DBLEND_INVDESTALPHA: return D3D11_BLEND_INV_DEST_ALPHA;
    case D3DBLEND_DESTCOLOR: return D3D11_BLEND_DEST_COLOR;
    case D3DBLEND_INVDESTCOLOR: return D3D11_BLEND_INV_DEST_COLOR;
    case D3DBLEND_SRCALPHASAT: return D3D11_BLEND_SRC_ALPHA_SAT;
    default: return D3D11_BLEND_ONE;
    }
}

D3D11_BLEND AlphaBlend(D3D11_BLEND b) {
    switch (b) {
    case D3D11_BLEND_SRC_COLOR: return D3D11_BLEND_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_COLOR: return D3D11_BLEND_INV_SRC_ALPHA;
    case D3D11_BLEND_DEST_COLOR: return D3D11_BLEND_DEST_ALPHA;
    case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
    default: return b;
    }
}

ID3D11BlendState* BlendFor(bool enable, DWORD src, DWORD dst, bool writeColor = true) {
    uint64_t key = (uint64_t)enable | ((uint64_t)src << 1) | ((uint64_t)dst << 9) | ((uint64_t)writeColor << 17);
    auto it = g_blends.find(key);
    if (it != g_blends.end()) return it->second;
    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = enable;
    rt.SrcBlend = MapBlend(src);
    rt.DestBlend = MapBlend(dst);
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = AlphaBlend(rt.SrcBlend);
    rt.DestBlendAlpha = AlphaBlend(rt.DestBlend);
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = writeColor ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;
    ID3D11BlendState* s = nullptr;
    g_dev->CreateBlendState(&bd, &s);
    return g_blends[key] = s;
}

ID3D11DepthStencilState* DepthFor(bool enable, bool write, DWORD func) {
    uint64_t key = (uint64_t)enable | ((uint64_t)write << 1) | ((uint64_t)func << 2);
    auto it = g_depths.find(key);
    if (it != g_depths.end()) return it->second;
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = enable;
    dd.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = (func >= 1 && func <= 8) ? (D3D11_COMPARISON_FUNC)func : D3D11_COMPARISON_LESS_EQUAL;
    ID3D11DepthStencilState* s = nullptr;
    g_dev->CreateDepthStencilState(&dd, &s);
    return g_depths[key] = s;
}

ID3D11RasterizerState* RasterFor(DWORD cull, DWORD fill, int bias, bool scissor) {
    uint64_t key = (uint64_t)cull | ((uint64_t)fill << 4) | ((uint64_t)(bias & 0xFF) << 8) | ((uint64_t)scissor << 16);
    auto it = g_rasters.find(key);
    if (it != g_rasters.end()) return it->second;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = fill == D3DFILL_WIREFRAME ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    // D3D6 culls by screen-space winding: CW culls clockwise faces, CCW culls counter-clockwise.
    rd.CullMode = cull == D3DCULL_CW ? D3D11_CULL_FRONT : cull == D3DCULL_CCW ? D3D11_CULL_BACK : D3D11_CULL_NONE;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthBias = -bias * 16;
    rd.DepthClipEnable = FALSE;
    rd.ScissorEnable = scissor;
    ID3D11RasterizerState* s = nullptr;
    g_dev->CreateRasterizerState(&rd, &s);
    return g_rasters[key] = s;
}

D3D11_TEXTURE_ADDRESS_MODE MapAddress(DWORD a) {
    switch (a) {
    case D3DTADDRESS_MIRROR: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case D3DTADDRESS_CLAMP: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case D3DTADDRESS_BORDER: return D3D11_TEXTURE_ADDRESS_BORDER;
    default: return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

ID3D11SamplerState* SamplerFor(const DWORD* t) {
    DWORD mag = t[D3DTSS_MAGFILTER], min = t[D3DTSS_MINFILTER], mip = t[D3DTSS_MIPFILTER];
    int forced = GetConfig().textureFilter;
    if (forced == 0) { mag = D3DTFG_POINT; min = D3DTFN_POINT; }
    else if (forced == 1) { mag = D3DTFG_LINEAR; min = D3DTFN_LINEAR; if (mip == D3DTFP_POINT) mip = D3DTFP_LINEAR; }
    else if (forced == 2) { mag = D3DTFG_ANISOTROPIC; min = D3DTFN_ANISOTROPIC; if (mip != D3DTFP_NONE) mip = D3DTFP_LINEAR; }
    uint64_t key = (uint64_t)mag | ((uint64_t)min << 4) | ((uint64_t)mip << 8) |
                   ((uint64_t)t[D3DTSS_ADDRESSU] << 12) | ((uint64_t)t[D3DTSS_ADDRESSV] << 16);
    auto it = g_samplers.find(key);
    if (it != g_samplers.end()) return it->second;
    D3D11_SAMPLER_DESC sd{};
    bool aniso = mag == D3DTFG_ANISOTROPIC || min == D3DTFN_ANISOTROPIC;
    bool linMag = mag != D3DTFG_POINT, linMin = min != D3DTFN_POINT, linMip = mip == D3DTFP_LINEAR;
    if (aniso) sd.Filter = D3D11_FILTER_ANISOTROPIC;
    else sd.Filter = (D3D11_FILTER)((linMin ? 0x10 : 0) | (linMag ? 0x4 : 0) | (linMip ? 0x1 : 0));
    sd.MaxAnisotropy = aniso ? 16 : 1;
    sd.AddressU = MapAddress(t[D3DTSS_ADDRESSU]);
    sd.AddressV = MapAddress(t[D3DTSS_ADDRESSV]);
    sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = mip == D3DTFP_NONE ? 0.0f : D3D11_FLOAT32_MAX;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ID3D11SamplerState* s = nullptr;
    g_dev->CreateSamplerState(&sd, &s);
    return g_samplers[key] = s;
}

void UnpackColor(DWORD c, float out[4]) {
    out[0] = ((c >> 16) & 0xFF) / 255.0f;
    out[1] = ((c >> 8) & 0xFF) / 255.0f;
    out[2] = (c & 0xFF) / 255.0f;
    out[3] = (c >> 24) / 255.0f;
}

void FillStage(const DWORD* t, Surface* tex, uint32_t a[4], uint32_t b[4]) {
    a[0] = t[D3DTSS_COLOROP];
    a[1] = t[D3DTSS_COLORARG1];
    a[2] = t[D3DTSS_COLORARG2];
    a[3] = t[D3DTSS_ALPHAOP];
    b[0] = t[D3DTSS_ALPHAARG1];
    b[1] = t[D3DTSS_ALPHAARG2];
    b[2] = tex ? 1 : 0;
    b[3] = tex && (tex->format.a || tex->TopMip()->hasSrcKey) ? 1 : 0;
}

// Debug aid: while a file named sb2modern_capture exists in the game folder, the presented
// image is written to capture_NNNN.bmp once per second.
void MaybeCaptureFrame(Surface* front) {
    static uint64_t nextCheck = 0;
    if (g_presentCount < nextCheck) return;
    nextCheck = g_presentCount + 60;
    std::wstring trigger = GameDir() + L"sb2modern_capture";
    if (GetFileAttributesW(trigger.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    D3D11_TEXTURE2D_DESC td{};
    front->store->tex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &staging))) return;
    g_ctx->CopyResource(staging, front->store->tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
        static int index = 0;
        wchar_t name[64];
        swprintf(name, 64, L"capture_%04d.bmp", index++);
        std::wstring path = GameDir() + name;
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        ih.biSize = sizeof(ih);
        ih.biWidth = td.Width;
        ih.biHeight = -(LONG)td.Height;
        ih.biPlanes = 1;
        ih.biBitCount = 32;
        DWORD imageBytes = td.Width * td.Height * 4;
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = fh.bfOffBits + imageBytes;
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (f) {
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&ih, sizeof(ih), 1, f);
            for (UINT y = 0; y < td.Height; ++y) fwrite((uint8_t*)m.pData + y * m.RowPitch, td.Width * 4, 1, f);
            fclose(f);
        }
        g_ctx->Unmap(staging, 0);
    }
    staging->Release();
}

void FrameLimit() {
    int limit = GetConfig().frameLimit;
    if (limit <= 0) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const LONGLONG interval = g_qpcFreq.QuadPart / limit;
    LONGLONG target = g_lastFrame.QuadPart + interval;
    LONGLONG remaining = target - now.QuadPart;
    if (remaining > 0) {
        // Sleep most of the remainder on a high-resolution timer, then spin the last fraction.
        LONGLONG sleep100ns = remaining * 10000000 / g_qpcFreq.QuadPart - 5000;
        if (sleep100ns > 0 && g_frameTimer) {
            LARGE_INTEGER due;
            due.QuadPart = -sleep100ns;
            SetWaitableTimer(g_frameTimer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(g_frameTimer, INFINITE);
        }
        do { QueryPerformanceCounter(&now); } while (now.QuadPart < target);
        g_lastFrame.QuadPart = target;
    } else {
        g_lastFrame = now;  // fell behind; don't try to catch up
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------
namespace Renderer {

bool Init() {
    if (g_initTried) return g_ok;
    g_initTried = true;

    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_lastFrame);
    g_frameTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!g_frameTimer) g_frameTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   levels, _countof(levels), D3D11_SDK_VERSION, &g_dev, nullptr, &ctx);
    if (FAILED(hr)) {
        LogWrite(1, "render", "no hardware D3D11 device (%08lX); using WARP", hr);
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               levels, _countof(levels), D3D11_SDK_VERSION, &g_dev, nullptr, &ctx);
    }
    if (FAILED(hr)) {
        LogWrite(1, "render", "D3D11CreateDevice failed: %08lX", hr);
        return false;
    }
    ctx->QueryInterface(IID_PPV_ARGS(&g_ctx));
    ctx->Release();
    if (!g_ctx) { LogWrite(1, "render", "D3D11.1 context unavailable"); return false; }

    IDXGIDevice* dxgiDev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    g_dev->QueryInterface(IID_PPV_ARGS(&dxgiDev));
    dxgiDev->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&g_factory));
    DXGI_ADAPTER_DESC ad;
    adapter->GetDesc(&ad);
    char name[128];
    WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name), nullptr, nullptr);
    LogWrite(1, "render", "Direct3D 11 on %s", name);
    adapter->Release();
    dxgiDev->Release();

    IDXGIFactory5* f5 = nullptr;
    if (SUCCEEDED(g_factory->QueryInterface(IID_PPV_ARGS(&f5)))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)))) g_tearing = allow != FALSE;
        f5->Release();
    }

    ID3DBlob *vs = nullptr, *ps = nullptr, *cvs = nullptr, *pvs = nullptr, *pps = nullptr;
    if (!Compile(kGameShader, "VSMain", "vs_4_0", &vs) || !Compile(kGameShader, "PSMain", "ps_4_0", &ps) ||
        !Compile(kGameShader, "VSClear", "vs_4_0", &cvs) || !Compile(kPresentShader, "VSMain", "vs_4_0", &pvs) ||
        !Compile(kPresentShader, "PSMain", "ps_4_0", &pps))
        return false;
    g_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs);
    g_dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps);
    g_dev->CreateVertexShader(cvs->GetBufferPointer(), cvs->GetBufferSize(), nullptr, &g_clearVs);
    g_dev->CreateVertexShader(pvs->GetBufferPointer(), pvs->GetBufferSize(), nullptr, &g_presentVs);
    g_dev->CreatePixelShader(pps->GetBufferPointer(), pps->GetBufferSize(), nullptr, &g_presentPs);
    const D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(GpuVertex, pos), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(GpuVertex, diffuse), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(GpuVertex, specular), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(GpuVertex, uv0), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(GpuVertex, uv1), D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    g_dev->CreateInputLayout(il, _countof(il), vs->GetBufferPointer(), vs->GetBufferSize(), &g_layout);
    for (ID3DBlob* b : {vs, ps, cvs, pvs, pps}) b->Release();

    g_vb = MakeBuffer(kVbVertices * sizeof(GpuVertex), D3D11_BIND_VERTEX_BUFFER);
    g_vsCb = MakeBuffer(sizeof(VSConst), D3D11_BIND_CONSTANT_BUFFER);
    g_psCb = MakeBuffer(sizeof(PSConst), D3D11_BIND_CONSTANT_BUFFER);
    g_presentCb = MakeBuffer(sizeof(PresentConst), D3D11_BIND_CONSTANT_BUFFER);

    D3D11_SAMPLER_DESC sd{};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    g_dev->CreateSamplerState(&sd, &g_presentPoint);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    g_dev->CreateSamplerState(&sd, &g_presentLinear);

    g_windowed = GetConfig().windowMode == 1;
    g_ok = true;
    return true;
}

ID3D11Device* Dev() { return g_dev; }
ID3D11DeviceContext1* Ctx() { return g_ctx; }
HWND Window() { return g_hwnd; }
uint64_t PresentCount() { return g_presentCount; }

void AttachWindow(HWND hwnd) {
    if (!Init() || hwnd == g_hwnd) return;
    if (g_hwnd && g_gameProc) SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_gameProc);
    SafeRelease(g_backRtv);
    SafeRelease(g_swap);
    g_hwnd = hwnd;
    g_gameProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)WndProc);

    DXGI_SWAP_CHAIN_DESC1 sc{};
    sc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sc.SampleDesc.Count = 1;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = 2;
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc.Flags = g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    HRESULT hr = g_factory->CreateSwapChainForHwnd(g_dev, hwnd, &sc, nullptr, nullptr, &g_swap);
    if (FAILED(hr)) {
        LogWrite(1, "render", "flip-model swap chain failed (%08lX); falling back to blit model", hr);
        sc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        sc.BufferCount = 1;
        sc.Flags = 0;
        g_tearing = false;
        g_factory->CreateSwapChainForHwnd(g_dev, hwnd, &sc, nullptr, nullptr, &g_swap);
    }
    g_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    ApplyWindowMode();
    LogWrite(1, "render", "attached to game window, %s, client %dx%d", g_windowed ? "windowed" : "borderless", g_clientW, g_clientH);
}

POINT ClientToGame(POINT c) {
    int dw = std::max<int>(1, g_dest.right - g_dest.left), dh = std::max<int>(1, g_dest.bottom - g_dest.top);
    LONG x = (LONG)((c.x - g_dest.left) * (LONGLONG)g_mode.width / dw);
    LONG y = (LONG)((c.y - g_dest.top) * (LONGLONG)g_mode.height / dh);
    return {std::clamp<LONG>(x, 0, g_mode.width - 1), std::clamp<LONG>(y, 0, g_mode.height - 1)};
}

bool IsInsideGameImage(POINT c) { return PtInRect(&g_dest, c) != FALSE; }

POINT GameToClient(POINT g) {
    int dw = g_dest.right - g_dest.left, dh = g_dest.bottom - g_dest.top;
    return {g_dest.left + (LONG)((g.x * (LONGLONG)dw + dw / 2) / g_mode.width),
            g_dest.top + (LONG)((g.y * (LONGLONG)dh + dh / 2) / g_mode.height)};
}

void CreateSurfaceResources(Surface* s) {
    Storage* st = s->store.get();
    if (st->tex || !g_dev) return;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = s->width;
    td.Height = s->height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;

    if (s->zbuffer) {
        td.Format = DXGI_FORMAT_D32_FLOAT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        g_dev->CreateTexture2D(&td, nullptr, &st->tex);
        if (st->tex) g_dev->CreateDepthStencilView(st->tex, nullptr, &st->dsv);
        if (st->dsv) g_ctx->ClearDepthStencilView(st->dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        st->where = Where::Gpu;
        return;
    }
    if (s->texture) {
        Surface* top = s->TopMip();
        if (top != s) { top->EnsureGpu(); return; }  // levels live in the top surface's texture
        int levels = 1;
        for (Surface* m = s; ; ++levels) {
            Surface* next = nullptr;
            for (Surface* a : m->attached) if (a->parentMip == s) { next = a; break; }
            if (!next) break;
            m = next;
        }
        td.MipLevels = levels;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        g_dev->CreateTexture2D(&td, nullptr, &st->tex);
        if (st->tex) g_dev->CreateShaderResourceView(st->tex, nullptr, &st->srv);
        return;
    }
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    g_dev->CreateTexture2D(&td, nullptr, &st->tex);
    if (st->tex) {
        g_dev->CreateShaderResourceView(st->tex, nullptr, &st->srv);
        g_dev->CreateRenderTargetView(st->tex, nullptr, &st->rtv);
    }
}

void Upload(Surface* s) {
    if (s->zbuffer) return;
    if (s->texture) {
        Surface* top = s->TopMip();
        top->EnsureGpu();
        if (!top->store->tex) return;
        const DDCOLORKEY* key = top->hasSrcKey ? &top->srcKey : nullptr;
        std::vector<uint32_t> buf;
        UINT level = 0;
        for (Surface* m = top; m; ++level) {
            buf.resize((size_t)m->width * m->height);
            ToBgra(m, m->store.get(), buf.data(), key);
            g_ctx->UpdateSubresource(top->store->tex, level, nullptr, buf.data(), m->width * 4, 0);
            m->store->where = Where::Both;
            Surface* next = nullptr;
            for (Surface* a : m->attached) if (a->parentMip == top) { next = a; break; }
            m = next;
        }
        top->textureDirty = false;
        return;
    }
    if (!s->store->tex) return;
    std::vector<uint32_t> buf((size_t)s->width * s->height);
    ToBgra(s, s->store.get(), buf.data(), nullptr);
    g_ctx->UpdateSubresource(s->store->tex, 0, nullptr, buf.data(), s->width * 4, 0);
}

void Readback(Surface* s) {
    Storage* st = s->store.get();
    if (!st->tex || s->zbuffer || s->texture) return;
    auto key = std::make_pair(s->width, s->height);
    ID3D11Texture2D*& staging = g_staging[key];
    if (!staging) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = s->width; td.Height = s->height; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        g_dev->CreateTexture2D(&td, nullptr, &staging);
    }
    g_ctx->CopyResource(staging, st->tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
        FromBgra(s, (const uint8_t*)m.pData, m.RowPitch);
        g_ctx->Unmap(staging, 0);
    }
}

void SubmitTriangles(Device* dev, const GpuVertex* v, size_t count, D3D11_PRIMITIVE_TOPOLOGY topo) {
    if (!g_ok) return;
    if (dev != g_batchDev || topo != g_batchTopo || g_batch.size() + count > kVbVertices) Flush();
    g_batchDev = dev;
    g_batchTopo = topo;
    g_batch.insert(g_batch.end(), v, v + count);
}

void Flush() {
    if (g_batch.empty() || !g_batchDev) { g_batch.clear(); return; }
    Device* dev = g_batchDev;
    Surface* rt = dev->target;
    Surface* z = rt->AttachedZ();

    rt->SyncToGpu();
    if (z) z->EnsureGpu();
    if (z && (z->width != rt->width || z->height != rt->height)) z = nullptr;
    if (!rt->store->rtv) { g_batch.clear(); return; }

    Surface* tex[2] = {dev->textures[0], dev->textures[1]};
    ID3D11ShaderResourceView* srvs[2] = {};
    ID3D11SamplerState* samps[2] = {};
    for (int i = 0; i < 2; ++i) {
        if (!tex[i]) continue;
        if (tex[i] == rt) { tex[i] = nullptr; continue; }
        Surface* top = tex[i]->TopMip();
        top->SyncToGpu();
        srvs[i] = top->store->srv ? top->store->srv : nullptr;
        if (!srvs[i]) { tex[i] = nullptr; continue; }
        samps[i] = SamplerFor(dev->tss[i]);
    }

    // Upload vertices into the ring buffer.
    UINT n = (UINT)g_batch.size();
    D3D11_MAP mapType = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (g_vbPos + n > kVbVertices) { g_vbPos = 0; mapType = D3D11_MAP_WRITE_DISCARD; }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(g_vb, 0, mapType, 0, &m))) { g_batch.clear(); return; }
    memcpy((GpuVertex*)m.pData + g_vbPos, g_batch.data(), n * sizeof(GpuVertex));
    g_ctx->Unmap(g_vb, 0);

    VSConst vc{{2.0f / rt->width, 2.0f / rt->height, 0, 0}};
    WriteCb(g_vsCb, vc);

    const DWORD* rs = dev->rs;
    PSConst pc{};
    FillStage(dev->tss[0], tex[0], pc.s0a, pc.s0b);
    FillStage(dev->tss[1], tex[1], pc.s1a, pc.s1b);
    if (!tex[0] && pc.s0a[0] != D3DTOP_DISABLE) {
        // No texture bound: stage 0 passes the diffuse colour through, as D3D6 drivers did.
        if (pc.s0a[1] == D3DTA_TEXTURE) pc.s0a[1] = D3DTA_DIFFUSE;
        if (pc.s0b[0] == D3DTA_TEXTURE) pc.s0b[0] = D3DTA_DIFFUSE;
    }
    uint32_t flags = 0;
    if (rs[D3DRENDERSTATE_COLORKEYENABLE]) flags |= 1;
    if (rs[D3DRENDERSTATE_ALPHATESTENABLE]) flags |= 2;
    if (rs[D3DRENDERSTATE_FOGENABLE]) flags |= 4;
    if (rs[D3DRENDERSTATE_SPECULARENABLE]) flags |= 8;
    if (rs[D3DRENDERSTATE_SHADEMODE] == D3DSHADE_FLAT) flags |= 16;
    pc.misc[0] = flags;
    pc.misc[1] = rs[D3DRENDERSTATE_ALPHAFUNC];
    UnpackColor(rs[D3DRENDERSTATE_TEXTUREFACTOR], pc.tfactor);
    UnpackColor(rs[D3DRENDERSTATE_FOGCOLOR], pc.fogColor);
    pc.alphaRef[0] = (rs[D3DRENDERSTATE_ALPHAREF] & 0xFF) / 255.0f;
    WriteCb(g_psCb, pc);

    D3D11_VIEWPORT vp{0, 0, (float)rt->width, (float)rt->height, 0, 1};
    D3D11_RECT sc{0, 0, rt->width, rt->height};
    if (dev->viewport) {
        const D3DVIEWPORT2& v = dev->viewport->vp;
        sc.left = std::max<LONG>(0, v.dwX);
        sc.top = std::max<LONG>(0, v.dwY);
        sc.right = std::min<LONG>(rt->width, v.dwX + v.dwWidth);
        sc.bottom = std::min<LONG>(rt->height, v.dwY + v.dwHeight);
    }

    g_ctx->OMSetRenderTargets(1, &rt->store->rtv, z ? z->store->dsv : nullptr);
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetScissorRects(1, &sc);
    g_ctx->RSSetState(RasterFor(rs[D3DRENDERSTATE_CULLMODE], rs[D3DRENDERSTATE_FILLMODE], (int)rs[D3DRENDERSTATE_ZBIAS], true));
    g_ctx->OMSetBlendState(BlendFor(rs[D3DRENDERSTATE_ALPHABLENDENABLE] != 0, rs[D3DRENDERSTATE_SRCBLEND], rs[D3DRENDERSTATE_DESTBLEND]), nullptr, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(DepthFor(z && rs[D3DRENDERSTATE_ZENABLE], z && rs[D3DRENDERSTATE_ZWRITEENABLE], rs[D3DRENDERSTATE_ZFUNC]), 0);
    UINT stride = sizeof(GpuVertex), offset = 0;
    g_ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &offset);
    g_ctx->IASetInputLayout(g_layout);
    g_ctx->IASetPrimitiveTopology(g_batchTopo);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &g_vsCb);
    g_ctx->PSSetShader(g_ps, nullptr, 0);
    g_ctx->PSSetConstantBuffers(1, 1, &g_psCb);
    g_ctx->PSSetShaderResources(0, 2, srvs);
    g_ctx->PSSetSamplers(0, 2, samps);
    g_ctx->Draw(n, g_vbPos);
    g_vbPos += n;

    ID3D11ShaderResourceView* none[2] = {};
    g_ctx->PSSetShaderResources(0, 2, none);
    rt->MarkGpuWritten();
    if (z) z->store->where = Where::Gpu;
    g_batch.clear();
}

void ClearRect(Surface* rt, Surface* z, const RECT& r, DWORD flags, D3DCOLOR color, float zval) {
    if (!g_ok) return;
    Flush();
    if (rt && (flags & D3DCLEAR_TARGET)) {
        rt->SyncToGpu();
        if (rt->store->rtv) {
            float c[4];
            UnpackColor(color, c);
            D3D11_RECT rc{r.left, r.top, r.right, r.bottom};
            g_ctx->ClearView(rt->store->rtv, c, &rc, 1);
            rt->MarkGpuWritten();
        }
    }
    if (z && (flags & D3DCLEAR_ZBUFFER)) {
        z->EnsureGpu();
        if (!z->store->dsv) return;
        if (r.left <= 0 && r.top <= 0 && r.right >= z->width && r.bottom >= z->height) {
            g_ctx->ClearDepthStencilView(z->store->dsv, D3D11_CLEAR_DEPTH, zval, 0);
        } else {
            // Partial clear: draw a full-screen triangle that writes depth inside a scissor.
            float cd[4] = {zval, 0, 0, 0};
            static ID3D11Buffer* clearCb = MakeBuffer(16, D3D11_BIND_CONSTANT_BUFFER);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(g_ctx->Map(clearCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, cd, 16); g_ctx->Unmap(clearCb, 0); }
            D3D11_VIEWPORT vp{0, 0, (float)z->width, (float)z->height, 0, 1};
            D3D11_RECT sc{r.left, r.top, r.right, r.bottom};
            g_ctx->OMSetRenderTargets(0, nullptr, z->store->dsv);
            g_ctx->RSSetViewports(1, &vp);
            g_ctx->RSSetScissorRects(1, &sc);
            g_ctx->RSSetState(RasterFor(D3DCULL_NONE, D3DFILL_SOLID, 0, true));
            g_ctx->OMSetDepthStencilState(DepthFor(true, true, D3DCMP_ALWAYS), 0);
            g_ctx->OMSetBlendState(BlendFor(false, D3DBLEND_ONE, D3DBLEND_ZERO, false), nullptr, 0xFFFFFFFF);
            g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_ctx->IASetInputLayout(nullptr);
            g_ctx->VSSetShader(g_clearVs, nullptr, 0);
            g_ctx->VSSetConstantBuffers(0, 1, &clearCb);
            g_ctx->PSSetShader(nullptr, nullptr, 0);
            g_ctx->Draw(3, 0);
        }
        z->store->where = Where::Gpu;
    }
}

void MarkPrimaryDirty(Surface* primary) {
    // A flipping primary is presented by Flip; only a single-buffered primary needs this.
    for (Surface* a : primary->attached) if (a->backbuffer) return;
    g_dirtyPrimary = primary;
}

void PresentIfDirty() {
    if (g_dirtyPrimary) {
        Surface* p = g_dirtyPrimary;
        g_dirtyPrimary = nullptr;
        Present(p);
    }
}

void Present(Surface* front) {
    if (!g_ok || !g_swap) return;
    Flush();
    if (g_dirtyPrimary == front) g_dirtyPrimary = nullptr;
    front->SyncToGpu();
    if (!g_backRtv || !front->store->srv) return;

    const Config& cfg = GetConfig();
    float black[4] = {0, 0, 0, 1};
    g_ctx->ClearRenderTargetView(g_backRtv, black);

    float dw = (float)(g_dest.right - g_dest.left), dh = (float)(g_dest.bottom - g_dest.top);
    PresentConst pc{};
    pc.srcSize[0] = (float)front->width;  pc.srcSize[1] = (float)front->height;
    pc.srcSize[2] = 1.0f / front->width;  pc.srcSize[3] = 1.0f / front->height;
    pc.params[0] = (float)cfg.filter;
    pc.params[1] = dw / front->width;
    pc.params[2] = dh / front->height;
    WriteCb(g_presentCb, pc);

    D3D11_VIEWPORT vp{(float)g_dest.left, (float)g_dest.top, dw, dh, 0, 1};
    g_ctx->OMSetRenderTargets(1, &g_backRtv, nullptr);
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetState(RasterFor(D3DCULL_NONE, D3DFILL_SOLID, 0, false));
    g_ctx->OMSetBlendState(BlendFor(false, D3DBLEND_ONE, D3DBLEND_ZERO), nullptr, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(DepthFor(false, false, D3DCMP_ALWAYS), 0);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_presentVs, nullptr, 0);
    g_ctx->PSSetShader(g_presentPs, nullptr, 0);
    g_ctx->PSSetConstantBuffers(0, 1, &g_presentCb);
    g_ctx->PSSetShaderResources(0, 1, &front->store->srv);
    ID3D11SamplerState* samp = cfg.filter == 0 ? g_presentPoint : g_presentLinear;
    g_ctx->PSSetSamplers(0, 1, &samp);
    g_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    g_ctx->PSSetShaderResources(0, 1, &none);

    UINT interval = cfg.vsync ? 1 : 0;
    UINT flags = (!cfg.vsync && g_tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    g_swap->Present(interval, flags);
    MaybeCaptureFrame(front);
    FrameLimit();
    if (g_presentCount == 0) LogWrite(1, "render", "first frame presented");
    ++g_presentCount;
    Compat::OnFramePresented();
}

}  // namespace Renderer
