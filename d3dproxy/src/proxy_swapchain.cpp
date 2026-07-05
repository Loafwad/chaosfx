#include "proxy.h"
#include "effects.h"
#include "proxy_drawcall.h"
#include <dxgi1_2.h>

// IDXGIFactory vtable slot for CreateSwapChain
static constexpr int SLOT_CreateSwapChain        = 10;
// IDXGIFactory2 vtable slot for CreateSwapChainForHwnd
// IUnknown(3) + IDXGIObject(4) + IDXGIFactory(5) + IDXGIFactory1(2) + IDXGIFactory2 offset 1
static constexpr int SLOT_CreateSwapChainForHwnd = 15;

typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateSwapChain)(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateSwapChainForHwnd)(
    IDXGIFactory2*, IUnknown*, HWND,
    const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
    IDXGIOutput*, IDXGISwapChain1**);

static PFN_CreateSwapChain        g_OrigCreateSwapChain        = nullptr;
static PFN_CreateSwapChainForHwnd g_OrigCreateSwapChainForHwnd = nullptr;
static bool                       g_Hooked                     = false;

static void PatchSlot(void** vtable, int slot, void* newFn, void** outOrig)
{
    *outOrig = vtable[slot];
    DWORD old;
    VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old);
    vtable[slot] = newFn;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
}

// ── Present hook ──────────────────────────────────────────────────────────
// IDXGISwapChain vtable: IUnknown(0-2) + IDXGIObject(3-6) +
//   IDXGIDeviceSubObject(7) + IDXGISwapChain::Present = slot 8
static constexpr int SLOT_Present = 8;

typedef HRESULT (STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
static PFN_Present g_OrigPresent = nullptr;

static HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
    g_PresentCount.fetch_add(1, std::memory_order_relaxed);
    Proxy_ResetDrawCounter();

    // Hook draw calls lazily — runs every frame until installed
    if (g_CfxContext && !g_HooksInstalled.load(std::memory_order_acquire))
        Proxy_HookDrawCalls(g_CfxContext);

    // Initialize D3D11 effect objects on first call (idempotent CAS — runs only once)
    if (g_CfxDevice && g_CfxContext)
        chaosfx::effects::Initialize(g_CfxDevice, g_CfxContext, pSwapChain);

    chaosfx::effects::RenderFrame();

    return g_OrigPresent(pSwapChain, SyncInterval, Flags);
}

static void CaptureSwapChain(IDXGISwapChain* sc)
{
    // Only capture the first swapchain — that's the game's main one.
    if (InterlockedCompareExchange(&g_CfxReady, 1, 0) == 0) {
        g_CfxSwapChain = sc;
        g_CfxSwapChain->AddRef();
        OutputDebugStringA("[ChaosFXProxy] Swapchain captured, g_CfxReady=1\n");
        Proxy_HookPresent(sc);
        if (g_CfxContext)
            Proxy_HookDrawCalls(g_CfxContext);
    }
}

void Proxy_HookPresent(IDXGISwapChain* sc)
{
    if (g_OrigPresent) return; // already hooked
    void** vt = *reinterpret_cast<void***>(sc);
    PatchSlot(vt, SLOT_Present,
              reinterpret_cast<void*>(&Hook_Present),
              reinterpret_cast<void**>(&g_OrigPresent));
    OutputDebugStringA("[ChaosFXProxy] IDXGISwapChain::Present hooked\n");
}

static HRESULT STDMETHODCALLTYPE Hook_CreateSwapChain(
    IDXGIFactory* pFactory, IUnknown* pDevice,
    DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain)
{
    HRESULT hr = g_OrigCreateSwapChain(pFactory, pDevice, pDesc, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain)
        CaptureSwapChain(*ppSwapChain);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateSwapChainForHwnd(
    IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreen,
    IDXGIOutput* pOutput, IDXGISwapChain1** ppSwapChain)
{
    HRESULT hr = g_OrigCreateSwapChainForHwnd(pFactory, pDevice, hWnd, pDesc, pFullscreen, pOutput, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain)
        CaptureSwapChain(*ppSwapChain);
    return hr;
}

// Called from D3D11CreateDevice intercept once we have the device.
// Patches the IDXGIFactory vtable so we see the game's CreateSwapChain* call.
void Proxy_HookFactory(IUnknown* pDevice)
{
    if (g_Hooked) return;
    g_Hooked = true;

    IDXGIDevice*  dxgiDev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory* factory = nullptr;

    pDevice->QueryInterface(__uuidof(IDXGIDevice),  reinterpret_cast<void**>(&dxgiDev));
    if (dxgiDev) dxgiDev->GetAdapter(&adapter);
    if (adapter) adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory));

    if (factory) {
        void** vt = *reinterpret_cast<void***>(factory);
        PatchSlot(vt, SLOT_CreateSwapChain,
                  reinterpret_cast<void*>(&Hook_CreateSwapChain),
                  reinterpret_cast<void**>(&g_OrigCreateSwapChain));
        OutputDebugStringA("[ChaosFXProxy] IDXGIFactory::CreateSwapChain hooked\n");

        IDXGIFactory2* factory2 = nullptr;
        factory->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory2));
        if (factory2) {
            void** vt2 = *reinterpret_cast<void***>(factory2);
            PatchSlot(vt2, SLOT_CreateSwapChainForHwnd,
                      reinterpret_cast<void*>(&Hook_CreateSwapChainForHwnd),
                      reinterpret_cast<void**>(&g_OrigCreateSwapChainForHwnd));
            OutputDebugStringA("[ChaosFXProxy] IDXGIFactory2::CreateSwapChainForHwnd hooked\n");
            factory2->Release();
        }
        factory->Release();
    }

    if (adapter) adapter->Release();
    if (dxgiDev) dxgiDev->Release();
}
