#include "effects.h"
#include "log.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cmath>
#include <atomic>
#include <mutex>
#include <windows.h>

#pragma comment(lib, "d3dcompiler.lib")

// ============================================================
// Embedded HLSL — compiled at runtime via D3DCompile so the
// build has no shader tool dependency during development.
// ============================================================

static const char* k_VertexShaderSrc = R"hlsl(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID)
{
    // Full-screen triangle from vertex ID (no vertex buffer needed)
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)hlsl";

// ----------------------------------------------------------
// Pink Mode: desaturate then tint with strong pink
// ----------------------------------------------------------
static const char* k_PinkShaderSrc = R"hlsl(
Texture2D    BackBuffer : register(t0);
SamplerState LinearSampler : register(s0);

cbuffer EffectCB : register(b0) {
    float intensity;
    float time;
    float2 _pad;
};

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

float4 main(VSOut input) : SV_Target
{
    float4 c    = BackBuffer.Sample(LinearSampler, input.uv);
    float  luma = dot(c.rgb, float3(0.2126, 0.7152, 0.0722));

    // Desaturate fully, then tint pink
    float3 pink  = float3(1.0, 0.18, 0.55);
    float3 tinted = lerp(c.rgb, luma * pink, intensity);

    // Subtle animated shimmer so it reads as an active effect
    float shimmer = sin(input.uv.y * 80.0 + time * 4.0) * 0.04 * intensity;
    tinted += shimmer;

    return float4(saturate(tinted), c.a);
}
)hlsl";

// ----------------------------------------------------------
// Kaleidoscope: polar UV folding into N symmetric segments
// ----------------------------------------------------------
static const char* k_KaleidoscopeShaderSrc = R"hlsl(
Texture2D    BackBuffer : register(t0);
SamplerState LinearSampler : register(s0);

cbuffer EffectCB : register(b0) {
    float intensity;
    float time;
    float2 _pad;
};

#define PI 3.14159265

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

float4 main(VSOut input) : SV_Target
{
    float2 centered = input.uv - float2(0.5, 0.5);
    centered.x     *= 1.0; // aspect handled by shader — square UV space

    float r     = length(centered);
    float theta = atan2(centered.y, centered.x);

    float segments   = 8.0;
    float angleStep  = PI * 2.0 / segments;
    float rotated    = theta + time * 0.3 * intensity;
    float folded     = fmod(abs(rotated), angleStep);
    if (folded > angleStep * 0.5) folded = angleStep - folded;

    float2 newUV = float2(cos(folded), sin(folded)) * r + float2(0.5, 0.5);

    float4 a = BackBuffer.Sample(LinearSampler, newUV);
    float4 b = BackBuffer.Sample(LinearSampler, input.uv);
    return lerp(b, a, intensity);
}
)hlsl";

// ----------------------------------------------------------
// Mirror: fold left half onto right half with slight tint
// ----------------------------------------------------------
static const char* k_MirrorShaderSrc = R"hlsl(
Texture2D    BackBuffer : register(t0);
SamplerState LinearSampler : register(s0);

cbuffer EffectCB : register(b0) {
    float intensity;
    float time;
    float2 _pad;
};

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

float4 main(VSOut input) : SV_Target
{
    float2 uv = input.uv;
    float2 mirroredUV = float2(uv.x > 0.5 ? 1.0 - uv.x : uv.x, uv.y);

    float4 orig     = BackBuffer.Sample(LinearSampler, uv);
    float4 mirrored = BackBuffer.Sample(LinearSampler, mirroredUV);

    // Cool blue tint on the mirrored side so the effect is obvious
    float3 tinted = mirrored.rgb * float3(0.7, 0.85, 1.2);
    return lerp(orig, float4(saturate(tinted), mirrored.a), intensity);
}
)hlsl";

// ============================================================

namespace chaosfx::effects {

struct ConstantBuffer {
    float intensity;
    float time;
    float pad[2];
};

// ---------------------------------------------------------------------------
// Initialization state machine
//   Idle      → Initialize() called                 → Compiling
//   Compiling → FinalizeOnRenderThread() compiles    → Ready
//   Any stage → failure                             → Failed
// ---------------------------------------------------------------------------
enum class InitState { Idle, Compiling, Ready, Failed };
static std::atomic<InitState> g_InitState { InitState::Idle };

static ID3D11Device*           g_Device  = nullptr;
static ID3D11DeviceContext*    g_Context = nullptr;

static ID3D11VertexShader*     g_VS         = nullptr;
static ID3D11PixelShader*      g_PinkPS     = nullptr;
static ID3D11PixelShader*      g_KaleidoPS  = nullptr;
static ID3D11PixelShader*      g_MirrorPS   = nullptr;
static ID3D11SamplerState*     g_Sampler    = nullptr;
static ID3D11Buffer*           g_CB         = nullptr;
static ID3D11BlendState*       g_BlendState = nullptr;
static IDXGISwapChain*         g_SwapChain  = nullptr;

static std::atomic<EffectType> g_ActiveEffect { EffectType::None };
static std::atomic<float>      g_Intensity    { 1.0f };

static ID3DBlob* CompileBlob(const char* src, const char* target)
{
    ID3DBlob* code   = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                            "main", target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL1, 0,
                            &code, &errors);
    if (errors) {
        CFXLOG("CompileBlob %s: %s", target,
               (const char*)errors->GetBufferPointer());
        errors->Release();
    }
    if (FAILED(hr)) return nullptr;
    return code;
}

// ---------------------------------------------------------------------------
// Called ONCE from the render thread on the first (and only) Present hook fire.
// Compiles shaders and creates all D3D11 objects in one shot.
// ---------------------------------------------------------------------------
bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain)
{
    InitState expected = InitState::Idle;
    if (!g_InitState.compare_exchange_strong(expected, InitState::Compiling))
        return true;

    g_Device    = device;    device->AddRef();
    g_Context   = context;   context->AddRef();
    g_SwapChain = swapChain; swapChain->AddRef();
    CFXLOG("Initialize: device=%p ctx=%p sc=%p", device, context, swapChain);

    CFXLOG("Initialize: compiling shaders");
    ID3DBlob* vsBlob      = CompileBlob(k_VertexShaderSrc,       "vs_5_0");
    ID3DBlob* pinkBlob    = CompileBlob(k_PinkShaderSrc,         "ps_5_0");
    ID3DBlob* kaleidoBlob = CompileBlob(k_KaleidoscopeShaderSrc, "ps_5_0");
    ID3DBlob* mirrorBlob  = CompileBlob(k_MirrorShaderSrc,       "ps_5_0");

    if (!vsBlob || !pinkBlob || !kaleidoBlob || !mirrorBlob) {
        CFXLOG("Initialize: shader compile FAILED");
        if (vsBlob)      vsBlob->Release();
        if (pinkBlob)    pinkBlob->Release();
        if (kaleidoBlob) kaleidoBlob->Release();
        if (mirrorBlob)  mirrorBlob->Release();
        g_InitState.store(InitState::Failed);
        return false;
    }
    CFXLOG("Initialize: shaders compiled, creating D3D11 objects");

    auto safeCreatePS = [](ID3DBlob* blob, ID3D11PixelShader** out) {
        HRESULT hr = g_Device->CreatePixelShader(
            blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out);
        CFXLOG("  CreatePixelShader hr=0x%08X out=%p", (unsigned)hr, *out);
        blob->Release();
    };

    HRESULT hr = g_Device->CreateVertexShader(
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_VS);
    CFXLOG("  CreateVertexShader hr=0x%08X", (unsigned)hr);
    vsBlob->Release();

    safeCreatePS(pinkBlob,    &g_PinkPS);
    safeCreatePS(kaleidoBlob, &g_KaleidoPS);
    safeCreatePS(mirrorBlob,  &g_MirrorPS);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    hr = g_Device->CreateSamplerState(&sd, &g_Sampler);
    CFXLOG("  CreateSamplerState hr=0x%08X", (unsigned)hr);

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = sizeof(ConstantBuffer);
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = g_Device->CreateBuffer(&cbd, nullptr, &g_CB);
    CFXLOG("  CreateBuffer hr=0x%08X", (unsigned)hr);

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable           = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = g_Device->CreateBlendState(&bd, &g_BlendState);
    CFXLOG("  CreateBlendState hr=0x%08X", (unsigned)hr);

    g_InitState.store(InitState::Ready);
    CFXLOG("Initialize: done, state=Ready");
    return true;
}

void Shutdown()
{
    CFXLOG("Shutdown called, state=%d", (int)g_InitState.load());
    g_InitState.store(InitState::Idle);

    auto safeRelease = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
    safeRelease(g_VS);
    safeRelease(g_PinkPS);
    safeRelease(g_KaleidoPS);
    safeRelease(g_MirrorPS);
    safeRelease(g_Sampler);
    safeRelease(g_CB);
    safeRelease(g_BlendState);
    safeRelease(g_SwapChain);
    safeRelease(g_Context);
    safeRelease(g_Device);
    CFXLOG("Shutdown done");
}

void SetEffect(EffectType type, float intensity)
{
    CFXLOG("SetEffect: type=%d intensity=%.2f", (int)type, intensity);
    g_ActiveEffect.store(type);
    g_Intensity.store(intensity);
}

void ClearEffect()
{
    CFXLOG("ClearEffect called");
    g_ActiveEffect.store(EffectType::None);
}

bool RenderFrame()
{
    static int s_CallCount = 0;
    if (s_CallCount++ < 5)
        CFXLOG("RenderFrame called #%d state=%d effect=%d sc=%p",
               s_CallCount, (int)g_InitState.load(), (int)g_ActiveEffect.load(), g_SwapChain);

    if (g_InitState.load() != InitState::Ready) return false;
    if (!g_SwapChain) return false;

    EffectType effect = g_ActiveEffect.load();
    if (effect == EffectType::None) return true;

    static int s_LogCount = 0;
    if (s_LogCount < 5) {
        CFXLOG("RenderFrame: effect=%d sc=%p dev=%p ctx=%p", (int)effect, g_SwapChain, g_Device, g_Context);
        ++s_LogCount;
    }

    ID3D11PixelShader* ps = nullptr;
    switch (effect) {
        case EffectType::PinkMode:     ps = g_PinkPS;    break;
        case EffectType::Kaleidoscope: ps = g_KaleidoPS; break;
        case EffectType::Mirror:       ps = g_MirrorPS;  break;
        default: return true;
    }

    // Get the backbuffer from our saved swapchain
    ID3D11Texture2D* backbufferTex = nullptr;
    HRESULT hrGB = g_SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&backbufferTex));
    if (FAILED(hrGB)) {
        CFXLOG("RenderFrame: GetBuffer FAILED hr=0x%08X", (unsigned)hrGB);
        return false;
    }
    ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(g_Device->CreateRenderTargetView(backbufferTex, nullptr, &rtv))) {
        backbufferTex->Release();
        return false;
    }

    // Copy the backbuffer so we can sample it while writing to it
    D3D11_TEXTURE2D_DESC desc = {};
    backbufferTex->GetDesc(&desc);
    desc.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
    desc.Usage          = D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags      = 0; // strip shared/keyed-mutex flags that block CreateTexture2D
    // SRGB formats can't be used as SRV with typeless — strip SRGB to get the base format
    if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    ID3D11Texture2D* copyTex = nullptr;
    if (FAILED(g_Device->CreateTexture2D(&desc, nullptr, &copyTex))) {
        rtv->Release(); backbufferTex->Release(); return false;
    }
    g_Context->CopyResource(copyTex, backbufferTex);

    ID3D11ShaderResourceView* srv = nullptr;
    g_Device->CreateShaderResourceView(copyTex, nullptr, &srv);

    // Update constant buffer
    static float s_Time = 0.0f;
    s_Time += 0.016f; // approximate frame dt
    ConstantBuffer cbData = { g_Intensity.load(), s_Time, 0, 0 };
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(g_Context->Map(g_CB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &cbData, sizeof(cbData));
        g_Context->Unmap(g_CB, 0);
    }

    // Save current render state so we restore it after our pass
    ID3D11RenderTargetView* prevRTV       = nullptr;
    ID3D11DepthStencilView* prevDSV       = nullptr;
    ID3D11VertexShader*     prevVS        = nullptr;
    ID3D11PixelShader*      prevPS        = nullptr;
    ID3D11BlendState*       prevBlend     = nullptr;
    float                   prevBlendFactor[4];
    UINT                    prevSampleMask = 0;
    D3D11_PRIMITIVE_TOPOLOGY prevTopology  = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    UINT                    prevVPCount   = 1;
    D3D11_VIEWPORT          prevVP        = {};

    g_Context->OMGetRenderTargets(1, &prevRTV, &prevDSV);
    g_Context->VSGetShader(&prevVS, nullptr, nullptr);
    g_Context->PSGetShader(&prevPS, nullptr, nullptr);
    g_Context->OMGetBlendState(&prevBlend, prevBlendFactor, &prevSampleMask);
    g_Context->IAGetPrimitiveTopology(&prevTopology);
    g_Context->RSGetViewports(&prevVPCount, &prevVP);

    // Draw our full-screen pass
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, float(desc.Width), float(desc.Height), 0.0f, 1.0f };
    g_Context->RSSetViewports(1, &vp);
    g_Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_Context->IASetInputLayout(nullptr);
    g_Context->VSSetShader(g_VS, nullptr, 0);
    g_Context->PSSetShader(ps, nullptr, 0);
    g_Context->PSSetShaderResources(0, 1, &srv);
    g_Context->PSSetSamplers(0, 1, &g_Sampler);
    g_Context->PSSetConstantBuffers(0, 1, &g_CB);
    g_Context->OMSetRenderTargets(1, &rtv, nullptr);
    g_Context->OMSetBlendState(g_BlendState, nullptr, 0xffffffff);
    g_Context->Draw(3, 0); // single triangle covers entire screen

    // Restore previous render state
    g_Context->RSSetViewports(prevVPCount, &prevVP);
    g_Context->OMSetRenderTargets(1, &prevRTV, prevDSV);
    g_Context->VSSetShader(prevVS, nullptr, 0);
    g_Context->PSSetShader(prevPS, nullptr, 0);
    g_Context->OMSetBlendState(prevBlend, prevBlendFactor, prevSampleMask);
    g_Context->IASetPrimitiveTopology(prevTopology);
    ID3D11ShaderResourceView* nullSRV = nullptr;
    g_Context->PSSetShaderResources(0, 1, &nullSRV);

    auto safeRelease = [](auto* p) { if (p) p->Release(); };
    safeRelease(prevRTV);
    safeRelease(prevDSV);
    safeRelease(prevVS);
    safeRelease(prevPS);
    safeRelease(prevBlend);
    safeRelease(srv);
    safeRelease(copyTex);
    safeRelease(rtv);
    safeRelease(backbufferTex);

    return true;
}

} // namespace chaosfx::effects
