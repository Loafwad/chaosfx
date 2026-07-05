#include "proxy.h"
#include "proxy_drawcall.h"
#include <d3d11_1.h>

// Helper: get a proc from the real d3d11.dll
template<typename T>
static T RealFn(const char* name)
{
    return reinterpret_cast<T>(GetProcAddress(g_RealD3D11, name));
}

// ── Forwarded exports (pass straight through) ─────────────────────────────

extern "C" HRESULT WINAPI D3D11CoreCreateDevice(
    IDXGIFactory* pFactory, IDXGIAdapter* pAdapter, UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
    ID3D11Device** ppDevice)
{
    auto fn = RealFn<decltype(&D3D11CoreCreateDevice)>("D3D11CoreCreateDevice");
    return fn ? fn(pFactory, pAdapter, Flags, pFeatureLevels, FeatureLevels, ppDevice) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D11CoreCreateLayeredDevice(
    const void* unknown0, DWORD unknown1, const void* unknown2, REFIID riid, void** ppvObj)
{
    auto fn = RealFn<HRESULT(WINAPI*)(const void*, DWORD, const void*, REFIID, void**)>(
        "D3D11CoreCreateLayeredDevice");
    return fn ? fn(unknown0, unknown1, unknown2, riid, ppvObj) : E_NOTIMPL;
}

extern "C" SIZE_T WINAPI D3D11CoreGetLayeredDeviceSize(const void* unknown0, DWORD unknown1)
{
    auto fn = RealFn<SIZE_T(WINAPI*)(const void*, DWORD)>("D3D11CoreGetLayeredDeviceSize");
    return fn ? fn(unknown0, unknown1) : 0;
}

extern "C" HRESULT WINAPI D3D11CoreRegisterLayers(const void* unknown0, DWORD unknown1)
{
    auto fn = RealFn<HRESULT(WINAPI*)(const void*, DWORD)>("D3D11CoreRegisterLayers");
    return fn ? fn(unknown0, unknown1) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D11On12CreateDevice(
    IUnknown* pDevice, UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
    IUnknown* const* ppCommandQueues, UINT NumQueues, UINT NodeMask,
    ID3D11Device** ppDevice, ID3D11DeviceContext** ppContext,
    D3D_FEATURE_LEVEL* pChosenFeatureLevel)
{
    auto fn = RealFn<decltype(&D3D11On12CreateDevice)>("D3D11On12CreateDevice");
    return fn ? fn(pDevice, Flags, pFeatureLevels, FeatureLevels, ppCommandQueues,
                   NumQueues, NodeMask, ppDevice, ppContext, pChosenFeatureLevel) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI EnableFeatureLevelUpgrade()
{
    auto fn = RealFn<HRESULT(WINAPI*)()>("EnableFeatureLevelUpgrade");
    return fn ? fn() : E_NOTIMPL;
}

extern "C" HRESULT WINAPI OpenAdapter10(void* pData)
{
    auto fn = RealFn<HRESULT(WINAPI*)(void*)>("OpenAdapter10");
    return fn ? fn(pData) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI OpenAdapter10_2(void* pData)
{
    auto fn = RealFn<HRESULT(WINAPI*)(void*)>("OpenAdapter10_2");
    return fn ? fn(pData) : E_NOTIMPL;
}

// ── Intercepted: D3D11CreateDevice ────────────────────────────────────────
// We capture the device and immediate context here, then hook the factory
// so we see the swapchain when it's created.

extern "C" HRESULT WINAPI D3D11CreateDevice(
    IDXGIAdapter*               pAdapter,
    D3D_DRIVER_TYPE             DriverType,
    HMODULE                     Software,
    UINT                        Flags,
    const D3D_FEATURE_LEVEL*    pFeatureLevels,
    UINT                        FeatureLevels,
    UINT                        SDKVersion,
    ID3D11Device**              ppDevice,
    D3D_FEATURE_LEVEL*          pFeatureLevel,
    ID3D11DeviceContext**       ppImmediateContext)
{
    auto fn = RealFn<decltype(&D3D11CreateDevice)>("D3D11CreateDevice");
    if (!fn) return E_NOTIMPL;

    HRESULT hr = fn(pAdapter, DriverType, Software,
                    Flags & ~D3D11_CREATE_DEVICE_DEBUG,  // no debug layer: keeps shared vtable
                    pFeatureLevels, FeatureLevels, SDKVersion,
                    ppDevice, pFeatureLevel, ppImmediateContext);

    if (SUCCEEDED(hr) && ppDevice && *ppDevice && !g_CfxDevice) {
        g_CfxDevice = *ppDevice;
        g_CfxDevice->AddRef();
        if (ppImmediateContext && *ppImmediateContext) {
            g_CfxContext = *ppImmediateContext;
            g_CfxContext->AddRef();
        } else {
            (*ppDevice)->GetImmediateContext(&g_CfxContext);
        }
        OutputDebugStringA("[ChaosFXProxy] D3D11CreateDevice captured, hooking factory\n");
        Proxy_HookShaderCreate(*ppDevice);
        Proxy_HookFactory(*ppDevice);
    }
    return hr;
}

// ── Intercepted: D3D11CreateDeviceAndSwapChain ────────────────────────────
// Some games create device+swapchain in one call — capture both here.

extern "C" HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter*               pAdapter,
    D3D_DRIVER_TYPE             DriverType,
    HMODULE                     Software,
    UINT                        Flags,
    const D3D_FEATURE_LEVEL*    pFeatureLevels,
    UINT                        FeatureLevels,
    UINT                        SDKVersion,
    const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
    IDXGISwapChain**            ppSwapChain,
    ID3D11Device**              ppDevice,
    D3D_FEATURE_LEVEL*          pFeatureLevel,
    ID3D11DeviceContext**       ppImmediateContext)
{
    auto fn = RealFn<decltype(&D3D11CreateDeviceAndSwapChain)>("D3D11CreateDeviceAndSwapChain");
    if (!fn) return E_NOTIMPL;

    HRESULT hr = fn(pAdapter, DriverType, Software,
                    Flags & ~D3D11_CREATE_DEVICE_DEBUG,  // no debug layer: keeps shared vtable
                    pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc,
                    ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);

    if (SUCCEEDED(hr) && !g_CfxDevice) {
        if (ppDevice && *ppDevice) {
            g_CfxDevice = *ppDevice;
            g_CfxDevice->AddRef();
            if (ppImmediateContext && *ppImmediateContext) {
                g_CfxContext = *ppImmediateContext;
                g_CfxContext->AddRef();
            } else {
                (*ppDevice)->GetImmediateContext(&g_CfxContext);
            }
        }
        if (ppSwapChain && *ppSwapChain && !g_CfxReady) {
            g_CfxSwapChain = *ppSwapChain;
            g_CfxSwapChain->AddRef();
            InterlockedExchange(&g_CfxReady, 1);
            OutputDebugStringA("[ChaosFXProxy] D3D11CreateDeviceAndSwapChain captured all\n");
            Proxy_HookShaderCreate(*ppDevice);
            Proxy_HookPresent(*ppSwapChain);
            if (g_CfxContext)
                Proxy_HookDrawCalls(g_CfxContext);
        } else if (g_CfxDevice) {
            Proxy_HookShaderCreate(g_CfxDevice);
            Proxy_HookFactory(g_CfxDevice);
        }
    }
    return hr;
}
