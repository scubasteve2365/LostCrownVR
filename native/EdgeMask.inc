// Edge mask (build 10): each eye shows only the world both eyes can see. Included by
// LostCrownVRNative.cpp after the eye maths.
//
// After both eyes render, on the game's render thread and before their images go to the headset, one full-screen pass
// per eye: each pixel's point is rebuilt from that eye's own depth buffer and projected into the other eye; if it falls
// outside the other eye's left or right edge, the pixel is painted with the Surround colour. Both eyes then end on the
// same world edge at every depth (a fixed strip cannot: the depth at an edge changes within one frame).
//
// The two eye cameras share the game camera's orientation and differ only by a shift (x, and z for head movement), so
// in view space: z from the depth (Unity's reversed depth, the GL projection's P22 and P23), x from the pixel column,
// then the shift to the other eye and its projection. Rows do not matter (no x-y terms in the projections).
// The HUD is drawn later into its own texture, so it is never masked. Menu cameras (flat, both eyes in the middle) give
// both eyes the same picture, so nothing is masked there.

namespace edgemask {

struct Params {
    float p00[2], p02[2];  // each eye's GL projection, x row
    float p22, p23;        // depth terms (the game camera's, the same for both eyes)
    float ex[2], es[2];    // each eye's shift from the game camera: x (right) and back along the view (z)
    float colour[4];       // Surround colour
    int on;
};
static Params g_ring[8];
static std::mutex g_mx;
static bool g_on = true;            // menu STAGE tab, Edge match: On / Off
static void* g_depthPtr[2];         // the eye textures' native depth buffers
static Il2CppObject* g_depthFor[2]; // the eye textures they belong to
typedef void* (*NativeDepth_t)(Il2CppObject*);
static NativeDepth_t g_nativeDepth;

static ID3D11Device* g_dev; static ID3D11DeviceContext* g_ctx;
static ID3D11VertexShader* g_vs; static ID3D11PixelShader* g_ps; static ID3D11Buffer* g_cb;
static ID3D11RasterizerState* g_rs; static ID3D11DepthStencilState* g_dss; static ID3D11BlendState* g_bs;
static ID3D11ShaderResourceView* g_srv[2]; static ID3D11RenderTargetView* g_rtv[2];
static void* g_srvFor[2]; static void* g_rtvFor[2];
static bool g_failed = false, g_logged = false;
static std::atomic<uint32_t> g_passes{0};

static const char* kShader = R"(
Texture2D<float> depthTex : register(t0);
cbuffer P : register(b0) { float4 a; float4 b; float4 c; float4 colour; };
// a: p00 this eye, p02 this eye, p00 other, p02 other; b: p22, p23, shift x (this minus other), shift z (this minus other)
// c: width, height, unused, unused
void vs(uint id : SV_VertexID, out float4 pos : SV_Position) {
    float2 uv = float2(id & 1, id >> 1);
    pos = float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target {
    float d = depthTex.Load(int3(pos.xy, 0));
    float denom = b.x + 1 - 2 * d;
    float z = denom != 0 ? -b.y / denom : -1e6;           // view z (negative in front)
    float xn = pos.x / c.x * 2 - 1;                         // this eye's column, -1 left .. 1 right
    float x = -z * (xn + a.y) / a.x;                         // view x in this eye
    float xo = x + b.z, zo = z + b.w;                        // the same point in the other eye's view
    float xno = zo < 0 ? (a.z * xo + a.w * zo) / (-zo) : 0;  // and its column there
    if (xno >= -1 && xno <= 1) discard;                      // the other eye sees it: keep
    return colour;
}
)";

static bool Create(ID3D11Device* dev) {
    g_dev = dev; dev->GetImmediateContext(&g_ctx);
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    pD3DCompile compile = dc ? (pD3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!compile) { Log("EDGEMASK no shader compiler: edge mask off"); return false; }
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    size_t n = strlen(kShader);
    if (FAILED(compile(kShader, n, "edgemask", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(compile(kShader, n, "edgemask", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err))) {
        Log("EDGEMASK shader error: %s", err ? (const char*)err->GetBufferPointer() : "?"); return false;
    }
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
    vsb->Release(); psb->Release();
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 64; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dev->CreateBuffer(&bd, nullptr, &g_cb);
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &g_rs);
    D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthEnable = FALSE; dd.StencilEnable = FALSE; dev->CreateDepthStencilState(&dd, &g_dss);
    D3D11_BLEND_DESC bl{}; bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL; dev->CreateBlendState(&bl, &g_bs);
    Log("EDGEMASK pass ready");
    return g_vs && g_ps && g_cb && g_rs && g_dss && g_bs;
}

static DXGI_FORMAT DepthSrvFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}
static DXGI_FORMAT ColourRtvFormat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : f == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : f;
}

struct State {  // what this pass changes, put back afterwards
    ID3D11RenderTargetView* rtv[8] = {}; ID3D11DepthStencilView* dsv = nullptr;
    D3D11_VIEWPORT vp[16]; UINT nvp = 16; ID3D11RasterizerState* rs = nullptr; ID3D11DepthStencilState* dss = nullptr; UINT sref = 0;
    ID3D11BlendState* bs = nullptr; float bf[4]; UINT sm = 0; ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* cb = nullptr; ID3D11ShaderResourceView* srv = nullptr; ID3D11InputLayout* il = nullptr; D3D11_PRIMITIVE_TOPOLOGY topo;
    void Save(ID3D11DeviceContext* c) {
        c->OMGetRenderTargets(8, rtv, &dsv); c->RSGetViewports(&nvp, vp); c->RSGetState(&rs); c->OMGetDepthStencilState(&dss, &sref);
        c->OMGetBlendState(&bs, bf, &sm); c->VSGetShader(&vs, nullptr, nullptr); c->PSGetShader(&ps, nullptr, nullptr);
        c->PSGetConstantBuffers(0, 1, &cb); c->PSGetShaderResources(0, 1, &srv); c->IAGetInputLayout(&il); c->IAGetPrimitiveTopology(&topo);
    }
    void Restore(ID3D11DeviceContext* c) {
        c->OMSetRenderTargets(8, rtv, dsv); c->RSSetViewports(nvp, vp); c->RSSetState(rs); c->OMSetDepthStencilState(dss, sref);
        c->OMSetBlendState(bs, bf, sm); c->VSSetShader(vs, nullptr, 0); c->PSSetShader(ps, nullptr, 0);
        c->PSSetConstantBuffers(0, 1, &cb); c->PSSetShaderResources(0, 1, &srv); c->IASetInputLayout(il); c->IASetPrimitiveTopology(topo);
        for (auto* r : rtv) if (r) r->Release(); if (dsv) dsv->Release(); if (rs) rs->Release(); if (dss) dss->Release(); if (bs) bs->Release();
        if (vs) vs->Release(); if (ps) ps->Release(); if (cb) cb->Release(); if (srv) srv->Release(); if (il) il->Release();
    }
};

// The eye textures' own depth buffers stay empty: URP draws each eye into its own targets and copies only the colour
// back (tested: eye depth all 0; _CameraDepthAttachment is URP's). So right after each eye camera
// (UniversalRenderPipeline.RenderCameraStack returns, its work submitted), URP's depth attachment is copied into this
// eye's own texture, before the next camera reuses it.
static ID3D11Texture2D* g_copy[2]; static void* g_copySrcFor[2]; static void* g_srcDepth[2];
static bool g_copyLogged[2];
static DXGI_FORMAT Typeless(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32G8X24_TYPELESS;
        case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24G8_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
        case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}
static void __stdcall OnCopyDepth(int id) {
    int e = id & 1;
    ID3D11Resource* res = (ID3D11Resource*)g_srcDepth[e];
    if (!res) return;
    ID3D11Texture2D* src = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&src)) || !src) return;
    D3D11_TEXTURE2D_DESC d; src->GetDesc(&d);
    if (!g_copy[e] || g_copySrcFor[e] != res) {
        if (g_copy[e]) { g_copy[e]->Release(); g_copy[e] = nullptr; }
        D3D11_TEXTURE2D_DESC c = d; c.Format = Typeless(d.Format); c.BindFlags = D3D11_BIND_SHADER_RESOURCE; c.MiscFlags = 0; c.CPUAccessFlags = 0; c.Usage = D3D11_USAGE_DEFAULT;
        ID3D11Device* dev = nullptr; src->GetDevice(&dev);
        if (c.Format == DXGI_FORMAT_UNKNOWN || d.SampleDesc.Count != 1 || FAILED(dev->CreateTexture2D(&c, nullptr, &g_copy[e]))) {
            if (!g_copyLogged[e]) { g_copyLogged[e] = true; Log("EDGEMASK eye %d: URP depth %ux%u format %d samples %u cannot be copied", e, d.Width, d.Height, (int)d.Format, d.SampleDesc.Count); }
            dev->Release(); src->Release(); return;
        }
        dev->Release(); g_copySrcFor[e] = res;
        if (!g_copyLogged[e]) { g_copyLogged[e] = true; Log("EDGEMASK eye %d: URP depth %ux%u format %d copied after the eye renders", e, d.Width, d.Height, (int)d.Format); }
    }
    ID3D11Device* dev = nullptr; src->GetDevice(&dev); ID3D11DeviceContext* ctx = nullptr; dev->GetImmediateContext(&ctx);
    ctx->CopyResource(g_copy[e], src);
    ctx->Release(); dev->Release(); src->Release();
    g_depthPtr[e] = g_copy[e];
}

static void* g_colourPtr[2];
static void __stdcall OnEvent(int id) {
    Params p; { std::lock_guard<std::mutex> lk(g_mx); p = g_ring[id & 7]; }
    if (!p.on || g_failed) return;
    ID3D11Texture2D* col[2] = {(ID3D11Texture2D*)g_colourPtr[0], (ID3D11Texture2D*)g_colourPtr[1]};
    if (!col[0] || !col[1] || !g_depthPtr[0] || !g_depthPtr[1]) return;
    if (!g_dev) { ID3D11Device* d = nullptr; col[0]->GetDevice(&d); if (!Create(d)) { g_failed = true; return; } d->Release(); }
    for (int e = 0; e < 2; e++) {
        if (g_srvFor[e] != g_depthPtr[e]) {
            if (g_srv[e]) { g_srv[e]->Release(); g_srv[e] = nullptr; }
            ID3D11Texture2D* dt = nullptr;
            if (FAILED(((ID3D11Resource*)g_depthPtr[e])->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&dt))) { g_failed = true; Log("EDGEMASK depth buffer is not a 2D texture: off"); return; }
            D3D11_TEXTURE2D_DESC d; dt->GetDesc(&d);
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = DepthSrvFormat(d.Format); sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
            if (sd.Format == DXGI_FORMAT_UNKNOWN || !(d.BindFlags & D3D11_BIND_SHADER_RESOURCE) || d.SampleDesc.Count != 1 || FAILED(g_dev->CreateShaderResourceView(dt, &sd, &g_srv[e]))) {
                Log("EDGEMASK depth buffer format %d bind 0x%X samples %u cannot be read: off", (int)d.Format, d.BindFlags, d.SampleDesc.Count);
                dt->Release(); g_failed = true; return;
            }
            dt->Release(); g_srvFor[e] = g_depthPtr[e];
        }
        if (g_rtvFor[e] != col[e]) {
            if (g_rtv[e]) { g_rtv[e]->Release(); g_rtv[e] = nullptr; }
            D3D11_TEXTURE2D_DESC d; col[e]->GetDesc(&d);
            D3D11_RENDER_TARGET_VIEW_DESC rd{}; rd.Format = ColourRtvFormat(d.Format); rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            if (FAILED(g_dev->CreateRenderTargetView(col[e], &rd, &g_rtv[e]))) { Log("EDGEMASK eye texture format %d: no render target view, off", (int)d.Format); g_failed = true; return; }
            g_rtvFor[e] = col[e];
        }
    }
    D3D11_TEXTURE2D_DESC cd; col[0]->GetDesc(&cd);
    State st; st.Save(g_ctx);
    D3D11_VIEWPORT vp{0, 0, (float)cd.Width, (float)cd.Height, 0, 1};
    g_ctx->RSSetViewports(1, &vp); g_ctx->RSSetState(g_rs); g_ctx->OMSetDepthStencilState(g_dss, 0);
    float bf[4] = {}; g_ctx->OMSetBlendState(g_bs, bf, 0xffffffff);
    g_ctx->IASetInputLayout(nullptr); g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g_ctx->VSSetShader(g_vs, nullptr, 0); g_ctx->PSSetShader(g_ps, nullptr, 0);
    for (int e = 0; e < 2; e++) {
        int o = 1 - e;
        float cb[16] = {p.p00[e], p.p02[e], p.p00[o], p.p02[o], p.p22, p.p23, p.ex[e] - p.ex[o], p.es[e] - p.es[o],
                        (float)cd.Width, (float)cd.Height, 0, 0, p.colour[0], p.colour[1], p.colour[2], p.colour[3]};
        g_ctx->UpdateSubresource(g_cb, 0, nullptr, cb, 0, 0);
        g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
        ID3D11ShaderResourceView* none = nullptr; g_ctx->PSSetShaderResources(0, 1, &none);
        g_ctx->OMSetRenderTargets(1, &g_rtv[e], nullptr);
        g_ctx->PSSetShaderResources(0, 1, &g_srv[e]);
        g_ctx->Draw(4, 0);
        g_ctx->PSSetShaderResources(0, 1, &none);
    }
    st.Restore(g_ctx);
    g_passes++;
    if (!g_logged) { g_logged = true; Log("EDGEMASK first pass drawn on %ux%u eye images", cd.Width, cd.Height); }
}

// Main thread, after both eyes rendered and before the frame is handed over.
static void Issue(int frame, const Params& p, Il2CppObject* rtL, Il2CppObject* rtR, void* colL, void* colR) {
    if (!g_nativeDepth) g_nativeDepth = (NativeDepth_t)il2cpp_resolve_icall("UnityEngine.RenderTexture::GetNativeDepthBufferPtr");
    if (!g_nativeDepth) return;
    g_colourPtr[0] = colL; g_colourPtr[1] = colR;
    int slot = frame & 7;
    { std::lock_guard<std::mutex> lk(g_mx); g_ring[slot] = p; }
    g_issue((void*)&OnEvent, 200 + slot);
}

// Main thread, right after URP has rendered one camera.
typedef int (*PropId_t)(Il2CppObject*); typedef Il2CppObject* (*GetGlobTex_t)(int);
static PropId_t g_propId; static GetGlobTex_t g_getGlob; static int g_depthId = -1;
static void AfterCamera(int e) {
    if (!g_nativeDepth) g_nativeDepth = (NativeDepth_t)il2cpp_resolve_icall("UnityEngine.RenderTexture::GetNativeDepthBufferPtr");
    if (!g_propId) { g_propId = (PropId_t)il2cpp_resolve_icall("UnityEngine.Shader::PropertyToID"); g_getGlob = (GetGlobTex_t)il2cpp_resolve_icall("UnityEngine.Shader::GetGlobalTextureImpl"); }
    if (!g_propId || !g_getGlob || !g_nativeDepth) return;
    if (g_depthId < 0) g_depthId = g_propId(il2cpp_string_new("_CameraDepthAttachment"));
    Il2CppObject* t = g_getGlob(g_depthId);
    void* p = t ? g_nativeDepth(t) : nullptr;
    if (!p) return;
    g_srcDepth[e] = p;
    g_issue((void*)&OnCopyDepth, 210 + e);
}

}  // namespace edgemask
