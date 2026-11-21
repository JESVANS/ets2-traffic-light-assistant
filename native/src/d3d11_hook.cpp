#include "d3d11_hook.hpp"

#include <windows.h>
#include <d3d11.h>
#include <cstring>

#include "MinHook.h"

namespace tlhud {

namespace {

typedef HRESULT(WINAPI* PFN_D3DCompile)(
    LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
    const D3D_SHADER_MACRO* pDefines, ID3DInclude* pInclude,
    LPCSTR pEntrypoint, LPCSTR pTarget, UINT Flags1, UINT Flags2,
    ID3DBlob** ppCode, ID3DBlob** ppErrorMsgs);

HMODULE g_d3dcompiler_mod = nullptr;
PFN_D3DCompile g_pfn_d3dcompile = nullptr;

bool EnsureD3DCompile() {
    if (g_pfn_d3dcompile) return true;
    const wchar_t* names[] = {L"d3dcompiler_47.dll", L"d3dcompiler_46.dll", L"d3dcompiler_43.dll"};
    for (const wchar_t* n : names) {
        HMODULE m = GetModuleHandleW(n);
        if (!m) m = LoadLibraryW(n);
        if (m) {
            FARPROC raw_fn = GetProcAddress(m, "D3DCompile");
            PFN_D3DCompile fn = nullptr;
            std::memcpy(&fn, &raw_fn, sizeof(fn));
            if (fn) {
                g_d3dcompiler_mod = m;
                g_pfn_d3dcompile = fn;
                return true;
            }
        }
    }
    return false;
}

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PFN_Present g_orig_present = nullptr;
PFN_ResizeBuffers g_orig_resize = nullptr;
void* g_target_present = nullptr;
void* g_target_resize = nullptr;

FrameRenderCallback g_callback = nullptr;
void* g_user = nullptr;

HANDLE g_init_thread = nullptr;
volatile LONG g_stopping = 0;
volatile LONG g_in_flight = 0;
bool g_mh_initialized = false;

CRITICAL_SECTION g_render_cs;
bool g_cs_init = false;

// D3D11 render state
IDXGISwapChain* g_swap_chain = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11InputLayout* g_layout = nullptr;
ID3D11Buffer* g_vb = nullptr;
ID3D11Texture2D* g_tex = nullptr;
ID3D11ShaderResourceView* g_srv = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11BlendState* g_blend = nullptr;
ID3D11DepthStencilState* g_depth = nullptr;
ID3D11RasterizerState* g_rast = nullptr;
HudPainter* g_painter = nullptr;
UINT g_vp_w = 0;
UINT g_vp_h = 0;

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

void ReleaseRenderTarget() {
    SafeRelease(g_rtv);
}

void ReleaseAllD3D() {
    ReleaseRenderTarget();
    SafeRelease(g_vb);
    SafeRelease(g_srv);
    SafeRelease(g_tex);
    SafeRelease(g_sampler);
    SafeRelease(g_blend);
    SafeRelease(g_depth);
    SafeRelease(g_rast);
    SafeRelease(g_layout);
    SafeRelease(g_vs);
    SafeRelease(g_ps);
    SafeRelease(g_ctx);
    SafeRelease(g_device);
    g_swap_chain = nullptr;
    g_vp_w = 0;
    g_vp_h = 0;
    if (g_painter) {
        delete g_painter;
        g_painter = nullptr;
    }
}

const char* kShaderSrc = R"(
struct VS_IN {
    float4 pos   : POSITION;
    float2 uv    : TEXCOORD0;
    float  alpha : COLOR0;
};
struct PS_IN {
    float4 pos   : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float  alpha : COLOR0;
};
PS_IN VSMain(VS_IN input) {
    PS_IN o;
    o.pos = input.pos;
    o.uv = input.uv;
    o.alpha = input.alpha;
    return o;
}
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);
float4 PSMain(PS_IN input) : SV_Target {
    float4 c = tex0.Sample(samp0, input.uv);
    return c * input.alpha;
}
)";

bool InitD3DResources(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(sc->GetDesc(&desc))) return false;
    if (!desc.OutputWindow || desc.BufferDesc.Width < 640 || desc.BufferDesc.Height < 360) {
        return false;
    }

    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_device)) || !g_device) {
        return false;
    }
    g_device->GetImmediateContext(&g_ctx);
    if (!g_ctx) {
        ReleaseAllD3D();
        return false;
    }

    if (!EnsureD3DCompile()) {
        ReleaseAllD3D();
        return false;
    }

    ID3DBlob* vs_blob = nullptr;
    ID3DBlob* ps_blob = nullptr;
    ID3DBlob* err_blob = nullptr;
    HRESULT hr = g_pfn_d3dcompile(kShaderSrc, std::strlen(kShaderSrc), nullptr, nullptr, nullptr,
                                  "VSMain", "vs_4_0", 0, 0, &vs_blob, &err_blob);
    SafeRelease(err_blob);
    if (FAILED(hr)) {
        SafeRelease(vs_blob);
        ReleaseAllD3D();
        return false;
    }

    hr = g_pfn_d3dcompile(kShaderSrc, std::strlen(kShaderSrc), nullptr, nullptr, nullptr,
                          "PSMain", "ps_4_0", 0, 0, &ps_blob, &err_blob);
    SafeRelease(err_blob);
    if (FAILED(hr)) {
        SafeRelease(vs_blob);
        SafeRelease(ps_blob);
        ReleaseAllD3D();
        return false;
    }

    hr = g_device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &g_vs);
    if (SUCCEEDED(hr)) {
        hr = g_device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &g_ps);
    }
    if (SUCCEEDED(hr)) {
        D3D11_INPUT_ELEMENT_DESC ied[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        hr = g_device->CreateInputLayout(ied, 3, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &g_layout);
    }
    SafeRelease(vs_blob);
    SafeRelease(ps_blob);
    if (FAILED(hr)) {
        ReleaseAllD3D();
        return false;
    }

    D3D11_BUFFER_DESC vbd{};
    vbd.ByteWidth = sizeof(HudVertex) * 4;
    vbd.Usage = D3D11_USAGE_DYNAMIC;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_device->CreateBuffer(&vbd, nullptr, &g_vb))) {
        ReleaseAllD3D();
        return false;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = HudPainter::kWidth;
    td.Height = HudPainter::kHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_device->CreateTexture2D(&td, nullptr, &g_tex))) {
        ReleaseAllD3D();
        return false;
    }
    if (FAILED(g_device->CreateShaderResourceView(g_tex, nullptr, &g_srv))) {
        ReleaseAllD3D();
        return false;
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(g_device->CreateSamplerState(&sd, &g_sampler))) {
        ReleaseAllD3D();
        return false;
    }

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(g_device->CreateBlendState(&bd, &g_blend))) {
        ReleaseAllD3D();
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(g_device->CreateDepthStencilState(&dsd, &g_depth))) {
        ReleaseAllD3D();
        return false;
    }

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = FALSE;
    rd.DepthClipEnable = FALSE;
    if (FAILED(g_device->CreateRasterizerState(&rd, &g_rast))) {
        ReleaseAllD3D();
        return false;
    }

    g_painter = new HudPainter();
    g_swap_chain = sc;
    return true;
}

bool EnsureRenderTarget(IDXGISwapChain* sc) {
    if (g_rtv) return true;
    ID3D11Texture2D* back_buf = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back_buf)) || !back_buf) {
        return false;
    }
    D3D11_TEXTURE2D_DESC bdesc{};
    back_buf->GetDesc(&bdesc);
    g_vp_w = bdesc.Width;
    g_vp_h = bdesc.Height;

    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
    rtv_desc.Format = bdesc.Format;
    // Handle SRGB backbuffer formats safely if needed
    if (bdesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        bdesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
        bdesc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
        bdesc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS) {
        rtv_desc.Format = bdesc.Format;
    }
    rtv_desc.ViewDimension = (bdesc.SampleDesc.Count > 1) ? D3D11_RTV_DIMENSION_TEXTURE2DMS
                                                        : D3D11_RTV_DIMENSION_TEXTURE2D;
    HRESULT hr = g_device->CreateRenderTargetView(back_buf, nullptr, &g_rtv);
    if (FAILED(hr)) {
        hr = g_device->CreateRenderTargetView(back_buf, &rtv_desc, &g_rtv);
    }
    back_buf->Release();
    return SUCCEEDED(hr) && g_rtv != nullptr;
}

struct SavedD3DState {
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11BlendState* blend = nullptr;
    float blend_factor[4]{};
    UINT sample_mask = 0xFFFFFFFF;
    ID3D11DepthStencilState* depth = nullptr;
    UINT stencil_ref = 0;
    ID3D11RasterizerState* rast = nullptr;
    UINT num_vp = 16;
    D3D11_VIEWPORT vps[16]{};
    UINT num_sc = 16;
    D3D11_RECT scs[16]{};
    ID3D11InputLayout* layout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer* vb = nullptr;
    UINT vb_stride = 0;
    UINT vb_offset = 0;
    ID3D11VertexShader* vs = nullptr;
    ID3D11ClassInstance* vs_inst[16]{};
    UINT vs_inst_cnt = 16;
    ID3D11PixelShader* ps = nullptr;
    ID3D11ClassInstance* ps_inst[16]{};
    UINT ps_inst_cnt = 16;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11ClassInstance* gs_inst[16]{};
    UINT gs_inst_cnt = 16;
    ID3D11ShaderResourceView* ps_srv = nullptr;
    ID3D11SamplerState* ps_samp = nullptr;

    void Save(ID3D11DeviceContext* ctx) {
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
        ctx->OMGetBlendState(&blend, blend_factor, &sample_mask);
        ctx->OMGetDepthStencilState(&depth, &stencil_ref);
        ctx->RSGetState(&rast);
        ctx->RSGetViewports(&num_vp, vps);
        ctx->RSGetScissorRects(&num_sc, scs);
        ctx->IAGetInputLayout(&layout);
        ctx->IAGetPrimitiveTopology(&topology);
        ctx->IAGetVertexBuffers(0, 1, &vb, &vb_stride, &vb_offset);
        ctx->VSGetShader(&vs, vs_inst, &vs_inst_cnt);
        ctx->PSGetShader(&ps, ps_inst, &ps_inst_cnt);
        ctx->GSGetShader(&gs, gs_inst, &gs_inst_cnt);
        ctx->PSGetShaderResources(0, 1, &ps_srv);
        ctx->PSGetSamplers(0, 1, &ps_samp);
    }

    void Restore(ID3D11DeviceContext* ctx) {
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
        ctx->OMSetBlendState(blend, blend_factor, sample_mask);
        ctx->OMSetDepthStencilState(depth, stencil_ref);
        ctx->RSSetState(rast);
        ctx->RSSetViewports(num_vp, vps);
        ctx->RSSetScissorRects(num_sc, scs);
        ctx->IASetInputLayout(layout);
        ctx->IASetPrimitiveTopology(topology);
        ctx->IASetVertexBuffers(0, 1, &vb, &vb_stride, &vb_offset);
        ctx->VSSetShader(vs, vs_inst, vs_inst_cnt);
        ctx->PSSetShader(ps, ps_inst, ps_inst_cnt);
        ctx->GSSetShader(gs, gs_inst, gs_inst_cnt);
        ctx->PSSetShaderResources(0, 1, &ps_srv);
        ctx->PSSetSamplers(0, 1, &ps_samp);

        for (auto*& r : rtvs) SafeRelease(r);
        SafeRelease(dsv);
        SafeRelease(blend);
        SafeRelease(depth);
        SafeRelease(rast);
        SafeRelease(layout);
        SafeRelease(vb);
        SafeRelease(vs);
        for (UINT i = 0; i < vs_inst_cnt && i < 16; ++i) SafeRelease(vs_inst[i]);
        SafeRelease(ps);
        for (UINT i = 0; i < ps_inst_cnt && i < 16; ++i) SafeRelease(ps_inst[i]);
        SafeRelease(gs);
        for (UINT i = 0; i < gs_inst_cnt && i < 16; ++i) SafeRelease(gs_inst[i]);
        SafeRelease(ps_srv);
        SafeRelease(ps_samp);
    }
};

void DrawOverlay(IDXGISwapChain* sc) {
    if (!g_callback) return;

    if (!g_swap_chain) {
        if (!InitD3DResources(sc)) return;
    } else if (sc != g_swap_chain) {
        return;
    }

    if (!EnsureRenderTarget(sc) || !g_painter) return;

    HudVertex verts[4]{};
    if (!g_callback((float)g_vp_w, (float)g_vp_h, verts, *g_painter, g_user)) {
        return;
    }

    if (g_painter->IsDirty()) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(g_ctx->Map(g_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            const uint8_t* src = g_painter->Pixels();
            uint8_t* dst = reinterpret_cast<uint8_t*>(mapped.pData);
            UINT row_bytes = (UINT)g_painter->StrideBytes();
            for (int y = 0; y < HudPainter::kHeight; ++y) {
                std::memcpy(dst + (size_t)y * mapped.RowPitch, src + (size_t)y * row_bytes, row_bytes);
            }
            g_ctx->Unmap(g_tex, 0);
            g_painter->ClearDirty();
        }
    }

    D3D11_MAPPED_SUBRESOURCE vmap{};
    if (FAILED(g_ctx->Map(g_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &vmap))) {
        return;
    }
    std::memcpy(vmap.pData, verts, sizeof(verts));
    g_ctx->Unmap(g_vb, 0);

    SavedD3DState saved;
    saved.Save(g_ctx);

    D3D11_VIEWPORT vp{};
    vp.Width = (float)g_vp_w;
    vp.Height = (float)g_vp_h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetState(g_rast);

    float bf[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->OMSetBlendState(g_blend, bf, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(g_depth, 0);

    UINT stride = sizeof(HudVertex);
    UINT offset = 0;
    g_ctx->IASetInputLayout(g_layout);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g_ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &offset);

    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->PSSetShader(g_ps, nullptr, 0);
    g_ctx->GSSetShader(nullptr, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &g_srv);
    g_ctx->PSSetSamplers(0, 1, &g_sampler);

    g_ctx->Draw(4, 0);

    saved.Restore(g_ctx);
}

HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* sc, UINT sync_interval, UINT flags) {
    InterlockedIncrement(&g_in_flight);
    if (!g_stopping && g_cs_init) {
        EnterCriticalSection(&g_render_cs);
        if (!g_stopping) {
            DrawOverlay(sc);
        }
        LeaveCriticalSection(&g_render_cs);
    }
    PFN_Present orig = g_orig_present;
    InterlockedDecrement(&g_in_flight);
    return orig ? orig(sc, sync_interval, flags) : S_OK;
}

HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* sc,
                                              UINT buffer_count,
                                              UINT width,
                                              UINT height,
                                              DXGI_FORMAT new_format,
                                              UINT swapchain_flags) {
    InterlockedIncrement(&g_in_flight);
    if (g_cs_init) {
        EnterCriticalSection(&g_render_cs);
        if (sc == g_swap_chain) {
            ReleaseRenderTarget();
        }
        LeaveCriticalSection(&g_render_cs);
    }
    PFN_ResizeBuffers orig = g_orig_resize;
    InterlockedDecrement(&g_in_flight);
    return orig ? orig(sc, buffer_count, width, height, new_format, swapchain_flags) : S_OK;
}

LRESULT CALLBACK DummyWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool GetSwapChainVtable(void*& out_present, void*& out_resize) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DummyWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ETS2TLHudDummyWnd";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                                0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    const D3D_DRIVER_TYPE drivers[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};

    HRESULT hr = E_FAIL;
    for (auto dt : drivers) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, dt, nullptr, 0, &fl, 1, D3D11_SDK_VERSION,
                                           &sd, &sc, &dev, nullptr, &ctx);
        if (SUCCEEDED(hr) && sc) break;
        SafeRelease(ctx);
        SafeRelease(dev);
        SafeRelease(sc);
    }

    bool ok = false;
    if (SUCCEEDED(hr) && sc) {
        void** vtable = *reinterpret_cast<void***>(sc);
        out_present = vtable[8];
        out_resize = vtable[13];
        ok = (out_present != nullptr && out_resize != nullptr);
    }

    SafeRelease(ctx);
    SafeRelease(dev);
    SafeRelease(sc);
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

DWORD WINAPI InitHookThread(LPVOID) {
    while (!g_stopping && !GetModuleHandleW(L"d3d11.dll")) {
        Sleep(100);
    }
    if (g_stopping) return 0;
    Sleep(300);

    void* present_addr = nullptr;
    void* resize_addr = nullptr;
    for (int attempt = 0; attempt < 20 && !g_stopping; ++attempt) {
        if (GetSwapChainVtable(present_addr, resize_addr)) break;
        Sleep(500);
    }
    if (g_stopping || !present_addr || !resize_addr) return 0;

    if (MH_Initialize() != MH_OK) return 0;
    g_mh_initialized = true;

    if (MH_CreateHook(present_addr, reinterpret_cast<void*>(&HookedPresent),
                      reinterpret_cast<void**>(&g_orig_present)) != MH_OK ||
        MH_CreateHook(resize_addr, reinterpret_cast<void*>(&HookedResizeBuffers),
                      reinterpret_cast<void**>(&g_orig_resize)) != MH_OK) {
        MH_Uninitialize();
        g_mh_initialized = false;
        return 0;
    }

    g_target_present = present_addr;
    g_target_resize = resize_addr;
    MH_EnableHook(MH_ALL_HOOKS);
    return 0;
}

} // namespace

bool StartD3D11Hook(FrameRenderCallback cb, void* user) {
    if (!g_cs_init) {
        InitializeCriticalSection(&g_render_cs);
        g_cs_init = true;
    }
    g_stopping = 0;
    g_callback = cb;
    g_user = user;
    g_init_thread = CreateThread(nullptr, 0, InitHookThread, nullptr, 0, nullptr);
    return g_init_thread != nullptr;
}

void StopD3D11Hook() {
    InterlockedExchange(&g_stopping, 1);
    if (g_init_thread) {
        WaitForSingleObject(g_init_thread, 2000);
        CloseHandle(g_init_thread);
        g_init_thread = nullptr;
    }

    if (g_mh_initialized) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        g_mh_initialized = false;
    }

    for (int i = 0; i < 100 && g_in_flight > 0; ++i) {
        Sleep(10);
    }

    if (g_cs_init) {
        EnterCriticalSection(&g_render_cs);
        ReleaseAllD3D();
        LeaveCriticalSection(&g_render_cs);
        DeleteCriticalSection(&g_render_cs);
        g_cs_init = false;
    }
    g_callback = nullptr;
    g_user = nullptr;
}

} // namespace tlhud
