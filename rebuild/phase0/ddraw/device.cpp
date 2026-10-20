#include "ddraw_impl.h"
#include <cstring>

Device::Device(DirectDraw* owner, Surface* rt) : dd(owner), target(rt) {
    impl = this;
    dd->AddRef();
    target->AddRef();
    target->renderTarget = true;
    SetDefaults();
}

Device::~Device() {
    Renderer::Flush();
    for (Surface* t : textures) if (t) t->Release();
    for (Viewport* v : viewports) { if (v->device == this) v->device = nullptr; v->Release(); }
    target->Release();
    dd->Release();
}

void Device::SetDefaults() {
    rs[D3DRENDERSTATE_ZENABLE] = target->AttachedZ() ? TRUE : FALSE;
    rs[D3DRENDERSTATE_ZWRITEENABLE] = TRUE;
    rs[D3DRENDERSTATE_ZFUNC] = D3DCMP_LESSEQUAL;
    rs[D3DRENDERSTATE_FILLMODE] = D3DFILL_SOLID;
    rs[D3DRENDERSTATE_SHADEMODE] = D3DSHADE_GOURAUD;
    rs[D3DRENDERSTATE_CULLMODE] = D3DCULL_CCW;
    rs[D3DRENDERSTATE_SRCBLEND] = D3DBLEND_ONE;
    rs[D3DRENDERSTATE_DESTBLEND] = D3DBLEND_ZERO;
    rs[D3DRENDERSTATE_ALPHAFUNC] = D3DCMP_ALWAYS;
    rs[D3DRENDERSTATE_TEXTUREPERSPECTIVE] = TRUE;
    rs[D3DRENDERSTATE_TEXTUREMAPBLEND] = D3DTBLEND_MODULATE;
    rs[D3DRENDERSTATE_TEXTUREMAG] = D3DFILTER_NEAREST;
    rs[D3DRENDERSTATE_TEXTUREMIN] = D3DFILTER_NEAREST;
    rs[D3DRENDERSTATE_TEXTUREFACTOR] = 0xFFFFFFFF;
    for (int s = 0; s < 8; ++s) {
        tss[s][D3DTSS_COLOROP] = s == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
        tss[s][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        tss[s][D3DTSS_COLORARG2] = D3DTA_CURRENT;
        tss[s][D3DTSS_ALPHAOP] = s == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
        tss[s][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        tss[s][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        tss[s][D3DTSS_TEXCOORDINDEX] = s;
        tss[s][D3DTSS_ADDRESS] = tss[s][D3DTSS_ADDRESSU] = tss[s][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
        tss[s][D3DTSS_MAGFILTER] = D3DTFG_POINT;
        tss[s][D3DTSS_MINFILTER] = D3DTFN_POINT;
        tss[s][D3DTSS_MIPFILTER] = D3DTFP_NONE;
    }
}

HRESULT Device::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3DDevice3) { *ppv = this; AddRef(); return S_OK; }
    LOG_VERBOSE("Device QI for unsupported interface %08lX", riid.Data1);
    *ppv = nullptr;
    return E_NOINTERFACE;
}
ULONG Device::AddRef() { return ++refs; }
ULONG Device::Release() { LONG r = --refs; if (!r) delete this; return (ULONG)r; }

HRESULT Device::GetCaps(LPD3DDEVICEDESC hw, LPD3DDEVICEDESC hel) {
    if (hw) FillDeviceDesc(hw);
    if (hel) FillDeviceDesc(hel);
    return D3D_OK;
}
HRESULT Device::GetStats(LPD3DSTATS s) { if (s) { DWORD size = s->dwSize; memset(s, 0, size); s->dwSize = size; } return D3D_OK; }

HRESULT Device::AddViewport(LPDIRECT3DVIEWPORT3 p) {
    auto v = (Viewport*)p;
    if (!v) return DDERR_INVALIDPARAMS;
    v->AddRef();
    v->device = this;
    viewports.push_back(v);
    return D3D_OK;
}
HRESULT Device::DeleteViewport(LPDIRECT3DVIEWPORT3 p) {
    for (auto it = viewports.begin(); it != viewports.end(); ++it) {
        if (*it == (Viewport*)p) {
            if (viewport == *it) viewport = nullptr;
            (*it)->device = nullptr;
            (*it)->Release();
            viewports.erase(it);
            return D3D_OK;
        }
    }
    return DDERR_INVALIDPARAMS;
}
HRESULT Device::NextViewport(LPDIRECT3DVIEWPORT3, LPDIRECT3DVIEWPORT3*, DWORD) { LOG_UNIMPL("NextViewport"); return DDERR_UNSUPPORTED; }

HRESULT Device::EnumTextureFormats(LPD3DENUMPIXELFORMATSCALLBACK cb, LPVOID ctx) {
    if (!cb) return DDERR_INVALIDPARAMS;
    const PixelFormat formats[] = {
        {16, 0xF800, 0x07E0, 0x001F, 0},          // R5G6B5
        {16, 0x7C00, 0x03E0, 0x001F, 0},          // X1R5G5B5
        {16, 0x7C00, 0x03E0, 0x001F, 0x8000},     // A1R5G5B5
        {16, 0x0F00, 0x00F0, 0x000F, 0xF000},     // A4R4G4B4
        {32, 0xFF0000, 0xFF00, 0xFF, 0},          // X8R8G8B8
        {32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000}, // A8R8G8B8
    };
    for (const PixelFormat& f : formats) {
        DDPIXELFORMAT pf = f.ToDD();
        if (cb(&pf, ctx) == D3DENUMRET_CANCEL) break;
    }
    return D3D_OK;
}

HRESULT Device::BeginScene() { inScene = true; return D3D_OK; }
HRESULT Device::EndScene() { inScene = false; Renderer::Flush(); return D3D_OK; }
HRESULT Device::GetDirect3D(LPDIRECT3D3* out) {
    if (!out) return DDERR_INVALIDPARAMS;
    dd->AddRef();
    *out = dd->d3d;
    return D3D_OK;
}
HRESULT Device::SetCurrentViewport(LPDIRECT3DVIEWPORT3 p) {
    Renderer::Flush();
    viewport = (Viewport*)p;
    if (viewport) viewport->device = this;
    return D3D_OK;
}
HRESULT Device::GetCurrentViewport(LPDIRECT3DVIEWPORT3* out) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = viewport;
    if (viewport) viewport->AddRef();
    return viewport ? D3D_OK : D3DERR_NOCURRENTVIEWPORT;
}
HRESULT Device::SetRenderTarget(LPDIRECTDRAWSURFACE4 p, DWORD) {
    Surface* s = Surface::From(p);
    if (!s) return DDERR_INVALIDPARAMS;
    Renderer::Flush();
    s->AddRef();
    s->renderTarget = true;
    target->Release();
    target = s;
    return D3D_OK;
}
HRESULT Device::GetRenderTarget(LPDIRECTDRAWSURFACE4* out) {
    if (!out) return DDERR_INVALIDPARAMS;
    target->AddRef();
    *out = target->v4;
    return D3D_OK;
}
HRESULT Device::Begin(D3DPRIMITIVETYPE, DWORD, DWORD) { LOG_UNIMPL("Device::Begin"); return DDERR_UNSUPPORTED; }
HRESULT Device::BeginIndexed(D3DPRIMITIVETYPE, DWORD, LPVOID, DWORD, DWORD) { LOG_UNIMPL("Device::BeginIndexed"); return DDERR_UNSUPPORTED; }
HRESULT Device::Vertex(LPVOID) { return DDERR_UNSUPPORTED; }
HRESULT Device::Index(WORD) { return DDERR_UNSUPPORTED; }
HRESULT Device::End(DWORD) { return DDERR_UNSUPPORTED; }

HRESULT Device::GetRenderState(D3DRENDERSTATETYPE s, LPDWORD v) {
    if (!v || (unsigned)s >= 256) return DDERR_INVALIDPARAMS;
    *v = rs[s];
    return D3D_OK;
}

// Translates the D3D5-era state into the stage-0 texture states the shader understands.
static void ApplyLegacyBlend(Device* d, DWORD mode) {
    DWORD* t = d->tss[0];
    t[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
    t[D3DTSS_COLORARG2] = D3DTA_DIFFUSE;
    t[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
    t[D3DTSS_ALPHAARG2] = D3DTA_DIFFUSE;
    switch (mode) {
    case D3DTBLEND_DECAL:
    case D3DTBLEND_COPY:
        t[D3DTSS_COLOROP] = D3DTOP_SELECTARG1;
        t[D3DTSS_ALPHAOP] = D3DTOP_SELECTARG1;
        break;
    case D3DTBLEND_DECALALPHA:
        t[D3DTSS_COLOROP] = D3DTOP_BLENDTEXTUREALPHA;
        t[D3DTSS_ALPHAOP] = D3DTOP_SELECTARG2;
        break;
    case D3DTBLEND_MODULATEALPHA:
        t[D3DTSS_COLOROP] = D3DTOP_MODULATE;
        t[D3DTSS_ALPHAOP] = D3DTOP_MODULATE;
        break;
    case D3DTBLEND_ADD:
        t[D3DTSS_COLOROP] = D3DTOP_ADD;
        t[D3DTSS_ALPHAOP] = D3DTOP_SELECTARG2;
        break;
    case D3DTBLEND_MODULATE:
    default:
        t[D3DTSS_COLOROP] = D3DTOP_MODULATE;
        t[D3DTSS_ALPHAOP] = 100;  // texture alpha if the texture has alpha, else diffuse alpha
        break;
    }
}

static void ApplyLegacyFilter(Device* d, DWORD mag, DWORD min) {
    DWORD* t = d->tss[0];
    t[D3DTSS_MAGFILTER] = mag == D3DFILTER_LINEAR ? D3DTFG_LINEAR : D3DTFG_POINT;
    switch (min) {
    case D3DFILTER_LINEAR:           t[D3DTSS_MINFILTER] = D3DTFN_LINEAR; t[D3DTSS_MIPFILTER] = D3DTFP_NONE; break;
    case D3DFILTER_MIPNEAREST:       t[D3DTSS_MINFILTER] = D3DTFN_POINT;  t[D3DTSS_MIPFILTER] = D3DTFP_POINT; break;
    case D3DFILTER_MIPLINEAR:        t[D3DTSS_MINFILTER] = D3DTFN_LINEAR; t[D3DTSS_MIPFILTER] = D3DTFP_POINT; break;
    case D3DFILTER_LINEARMIPNEAREST: t[D3DTSS_MINFILTER] = D3DTFN_POINT;  t[D3DTSS_MIPFILTER] = D3DTFP_LINEAR; break;
    case D3DFILTER_LINEARMIPLINEAR:  t[D3DTSS_MINFILTER] = D3DTFN_LINEAR; t[D3DTSS_MIPFILTER] = D3DTFP_LINEAR; break;
    default:                         t[D3DTSS_MINFILTER] = D3DTFN_POINT;  t[D3DTSS_MIPFILTER] = D3DTFP_NONE; break;
    }
}

HRESULT Device::SetRenderState(D3DRENDERSTATETYPE s, DWORD v) {
    if ((unsigned)s >= 256) return DDERR_INVALIDPARAMS;
    if (rs[s] == v) return D3D_OK;
    Renderer::Flush();
    rs[s] = v;
    switch (s) {
    case D3DRENDERSTATE_TEXTUREMAPBLEND: ApplyLegacyBlend(this, v); break;
    case D3DRENDERSTATE_TEXTUREMAG:
    case D3DRENDERSTATE_TEXTUREMIN: ApplyLegacyFilter(this, rs[D3DRENDERSTATE_TEXTUREMAG], rs[D3DRENDERSTATE_TEXTUREMIN]); break;
    case D3DRENDERSTATE_TEXTUREADDRESS: tss[0][D3DTSS_ADDRESSU] = tss[0][D3DTSS_ADDRESSV] = v; break;
    case D3DRENDERSTATE_TEXTUREADDRESSU: tss[0][D3DTSS_ADDRESSU] = v; break;
    case D3DRENDERSTATE_TEXTUREADDRESSV: tss[0][D3DTSS_ADDRESSV] = v; break;
    case D3DRENDERSTATE_TEXTUREHANDLE: {
        Surface* t = (Surface*)(uintptr_t)v;
        if (t) t->AddRef();
        if (textures[0]) textures[0]->Release();
        textures[0] = t;
        break;
    }
    default: break;
    }
    return D3D_OK;
}

HRESULT Device::GetLightState(D3DLIGHTSTATETYPE, LPDWORD v) { if (v) *v = 0; return D3D_OK; }
HRESULT Device::SetLightState(D3DLIGHTSTATETYPE, DWORD) { return D3D_OK; }
HRESULT Device::SetTransform(D3DTRANSFORMSTATETYPE, LPD3DMATRIX) { return D3D_OK; }
HRESULT Device::GetTransform(D3DTRANSFORMSTATETYPE, LPD3DMATRIX m) {
    if (m) { memset(m, 0, sizeof(*m)); m->_11 = m->_22 = m->_33 = m->_44 = 1.0f; }
    return D3D_OK;
}
HRESULT Device::MultiplyTransform(D3DTRANSFORMSTATETYPE, LPD3DMATRIX) { return D3D_OK; }

HRESULT Device::DrawPrimitive(D3DPRIMITIVETYPE type, DWORD fvf, LPVOID verts, DWORD count, DWORD) {
    return Draw(type, fvf, (const uint8_t*)verts, count, nullptr, 0);
}
HRESULT Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, DWORD fvf, LPVOID verts, DWORD count, LPWORD idx, DWORD idxCount, DWORD) {
    return Draw(type, fvf, (const uint8_t*)verts, count, idx, idxCount);
}
HRESULT Device::SetClipStatus(LPD3DCLIPSTATUS) { return D3D_OK; }
HRESULT Device::GetClipStatus(LPD3DCLIPSTATUS s) { if (s) memset(s, 0, sizeof(*s)); return D3D_OK; }
HRESULT Device::DrawPrimitiveStrided(D3DPRIMITIVETYPE, DWORD, LPD3DDRAWPRIMITIVESTRIDEDDATA, DWORD, DWORD) { LOG_UNIMPL("DrawPrimitiveStrided"); return DDERR_UNSUPPORTED; }
HRESULT Device::DrawIndexedPrimitiveStrided(D3DPRIMITIVETYPE, DWORD, LPD3DDRAWPRIMITIVESTRIDEDDATA, DWORD, LPWORD, DWORD, DWORD) { LOG_UNIMPL("DrawIndexedPrimitiveStrided"); return DDERR_UNSUPPORTED; }
HRESULT Device::DrawPrimitiveVB(D3DPRIMITIVETYPE, LPDIRECT3DVERTEXBUFFER, DWORD, DWORD, DWORD) { LOG_UNIMPL("DrawPrimitiveVB"); return DDERR_UNSUPPORTED; }
HRESULT Device::DrawIndexedPrimitiveVB(D3DPRIMITIVETYPE, LPDIRECT3DVERTEXBUFFER, LPWORD, DWORD, DWORD) { LOG_UNIMPL("DrawIndexedPrimitiveVB"); return DDERR_UNSUPPORTED; }
HRESULT Device::ComputeSphereVisibility(LPD3DVECTOR, LPD3DVALUE, DWORD, DWORD, LPDWORD) { return DDERR_UNSUPPORTED; }

HRESULT Device::GetTexture(DWORD stage, LPDIRECT3DTEXTURE2* out) {
    if (stage >= 8 || !out) return DDERR_INVALIDPARAMS;
    *out = textures[stage] ? textures[stage]->tex2 : nullptr;
    if (*out) (*out)->AddRef();
    return D3D_OK;
}
HRESULT Device::SetTexture(DWORD stage, LPDIRECT3DTEXTURE2 p) {
    if (stage >= 8) return DDERR_INVALIDPARAMS;
    Surface* t = Surface::From(p);
    if (t == textures[stage]) return D3D_OK;
    Renderer::Flush();
    if (t) t->AddRef();
    if (textures[stage]) textures[stage]->Release();
    textures[stage] = t;
    return D3D_OK;
}
HRESULT Device::GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, LPDWORD v) {
    if (stage >= 8 || (unsigned)type >= 32 || !v) return DDERR_INVALIDPARAMS;
    *v = tss[stage][type];
    return D3D_OK;
}
HRESULT Device::SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD v) {
    if (stage >= 8 || (unsigned)type >= 32) return DDERR_INVALIDPARAMS;
    if (tss[stage][type] == v) return D3D_OK;
    Renderer::Flush();
    tss[stage][type] = v;
    if (type == D3DTSS_ADDRESS) tss[stage][D3DTSS_ADDRESSU] = tss[stage][D3DTSS_ADDRESSV] = v;
    return D3D_OK;
}
HRESULT Device::ValidateDevice(LPDWORD passes) { if (passes) *passes = 1; return D3D_OK; }

// ---------------------------------------------------------------------------------------------
// Primitive conversion
// ---------------------------------------------------------------------------------------------
static inline void UnpackColor(DWORD c, float out[4]) {
    out[0] = ((c >> 16) & 0xFF) / 255.0f;
    out[1] = ((c >> 8) & 0xFF) / 255.0f;
    out[2] = (c & 0xFF) / 255.0f;
    out[3] = (c >> 24) / 255.0f;
}

HRESULT Device::Draw(D3DPRIMITIVETYPE type, DWORD fvf, const uint8_t* verts, DWORD count, const WORD* indices, DWORD indexCount) {
    if (!verts || !count) return D3D_OK;
    if ((fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW) {
        LOG_UNIMPL("DrawPrimitive with untransformed vertices");
        return D3D_OK;
    }

    // Field offsets in the game's vertex layout.
    size_t off = 16;
    if (fvf & D3DFVF_RESERVED1) off += 4;
    size_t diffuseOff = 0, specularOff = 0;
    if (fvf & D3DFVF_DIFFUSE) { diffuseOff = off; off += 4; }
    if (fvf & D3DFVF_SPECULAR) { specularOff = off; off += 4; }
    const DWORD texCount = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    const size_t texOff = off;
    const size_t stride = off + texCount * 8;

    // Which source texture coordinate set each stage reads.
    const DWORD tci0 = tss[0][D3DTSS_TEXCOORDINDEX] & 0xFFFF, tci1 = tss[1][D3DTSS_TEXCOORDINDEX] & 0xFFFF;

    auto fetch = [&](DWORD i, GpuVertex& v) {
        const uint8_t* p = verts + i * stride;
        memcpy(v.pos, p, 16);
        if (diffuseOff) UnpackColor(*(const DWORD*)(p + diffuseOff), v.diffuse);
        else v.diffuse[0] = v.diffuse[1] = v.diffuse[2] = v.diffuse[3] = 1.0f;
        if (specularOff) UnpackColor(*(const DWORD*)(p + specularOff), v.specular);
        else { v.specular[0] = v.specular[1] = v.specular[2] = 0.0f; v.specular[3] = 1.0f; }
        if (tci0 < texCount) memcpy(v.uv0, p + texOff + tci0 * 8, 8); else v.uv0[0] = v.uv0[1] = 0.0f;
        if (tci1 < texCount) memcpy(v.uv1, p + texOff + tci1 * 8, 8); else v.uv1[0] = v.uv1[1] = 0.0f;
    };

    const DWORD n = indices ? indexCount : count;
    auto idx = [&](DWORD k) -> DWORD { return indices ? indices[k] : k; };

    std::vector<DWORD> order;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    switch (type) {
    case D3DPT_TRIANGLELIST:
        for (DWORD k = 0; k + 2 < n; k += 3) { order.push_back(idx(k)); order.push_back(idx(k + 1)); order.push_back(idx(k + 2)); }
        break;
    case D3DPT_TRIANGLESTRIP:
        // Odd triangles are re-ordered to keep winding while leaving the flat-shading vertex first.
        for (DWORD k = 0; k + 2 < n; ++k) {
            order.push_back(idx(k));
            order.push_back(idx(k & 1 ? k + 2 : k + 1));
            order.push_back(idx(k & 1 ? k + 1 : k + 2));
        }
        break;
    case D3DPT_TRIANGLEFAN:
        // D3D shades fan triangles with their second vertex; rotate so it comes first.
        for (DWORD k = 1; k + 1 < n; ++k) { order.push_back(idx(k)); order.push_back(idx(k + 1)); order.push_back(idx(0)); }
        break;
    case D3DPT_LINELIST:
        topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        for (DWORD k = 0; k + 1 < n; k += 2) { order.push_back(idx(k)); order.push_back(idx(k + 1)); }
        break;
    case D3DPT_LINESTRIP:
        topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        for (DWORD k = 0; k + 1 < n; ++k) { order.push_back(idx(k)); order.push_back(idx(k + 1)); }
        break;
    case D3DPT_POINTLIST:
        topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
        for (DWORD k = 0; k < n; ++k) order.push_back(idx(k));
        break;
    default:
        return DDERR_INVALIDPARAMS;
    }
    if (order.empty()) return D3D_OK;

    std::vector<GpuVertex> out(order.size());
    for (size_t k = 0; k < order.size(); ++k) {
        if (order[k] >= count) return DDERR_INVALIDPARAMS;
        fetch(order[k], out[k]);
    }
    Renderer::SubmitTriangles(this, out.data(), out.size(), topo);
    return D3D_OK;
}
