#include "hook.h"
#include "log.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <intrin.h>
#include <mutex>

static constexpr int PRESENT_VTABLE_SLOT = 8;

namespace chaosfx::hook {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

// These symbols are accessed from hook_thunk.asm via EXTERN.
// They must have C linkage and no name mangling.
extern "C" {
    PresentFn cfx_OriginalPresent = nullptr;
    void      cfx_HookedPresentCallback(IDXGISwapChain* sc, UINT sync, UINT flags);
    HRESULT STDMETHODCALLTYPE cfx_CallOrigWithSEH(IDXGISwapChain* sc, UINT sync, UINT flags);
}

static void**     g_PresentSlot = nullptr;
static std::function<void(IDXGISwapChain*, UINT, UINT)> g_Callback;
static std::mutex g_CallbackMutex;

// Set by cfx_HookedPresentCallback before returning so that cfx_CallOrigWithSEH
// skips calling orig entirely on the one-shot hook frame. The Present call stack
// is already too deep to survive calling orig from inside our hook frame; we
// just return S_OK and let the game drop the first frame silently.
extern "C" volatile LONG cfx_SkipOrig = 0;

// forward-declared so cfx_HookedPresentCallback can call it
void Uninstall();

// Declared in hook_thunk.asm — this is what we write into vtable[8].
extern "C" HRESULT STDMETHODCALLTYPE HookedPresentThunk(IDXGISwapChain*, UINT, UINT);

// Called from the ASM thunk. Fires exactly once — grabs pointers via the callback,
// then immediately uninstalls the vtable patch so Openplanet's Present chain
// is completely unmodified from frame 2 onwards.
extern "C" void cfx_HookedPresentCallback(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    ULONG_PTR stackLow = 0, stackHigh = 0;
    GetCurrentThreadStackLimits(&stackLow, &stackHigh);
    ULONG_PTR approxRSP = (ULONG_PTR)_AddressOfReturnAddress();
    CFXLOG("cfx_HookedPresentCallback: tid=%lu sc=%p orig=%p freeKB=%llu",
           GetCurrentThreadId(), sc, cfx_OriginalPresent,
           (unsigned long long)(approxRSP - stackLow) / 1024);

    {
        std::lock_guard<std::mutex> lock(g_CallbackMutex);
        if (g_Callback) {
            g_Callback(sc, sync, flags);
            g_Callback = nullptr; // one-shot: never fire again
        }
    }

    // Signal cfx_CallOrigWithSEH to return S_OK immediately without calling orig.
    // Calling orig from inside our hook frame causes a stack overflow deep in
    // Openplanet's Present chain (~500ms later). We skip the one frame instead.
    InterlockedExchange(&cfx_SkipOrig, 1);

    // Restore the vtable immediately. From this point forward Openplanet's
    // Present chain runs completely unmodified — no hook, no instability.
    Uninstall();
    CFXLOG("cfx_HookedPresentCallback: vtable restored, hook gone");
}

// Calls orig inside an SEH frame so any exception is caught and logged
// instead of silently killing the process.
extern "C" HRESULT STDMETHODCALLTYPE cfx_CallOrigWithSEH(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    CFXLOG("cfx_CallOrigWithSEH: sc=%p sync=%u flags=%u", sc, sync, flags);

    // If the one-shot callback set this flag, skip orig entirely and return S_OK.
    // This avoids the stack overflow that occurs when orig (Openplanet's Present
    // chain) is called from inside our hook frame on the first captured frame.
    if (InterlockedCompareExchange(&cfx_SkipOrig, 0, 1) == 1) {
        CFXLOG("cfx_CallOrigWithSEH: SkipOrig set — returning S_OK, dropping one frame");
        return S_OK;
    }

    // Verify orig pointer is still in valid executable memory before calling it.
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery((LPCVOID)cfx_OriginalPresent, &mbi, sizeof(mbi))) {
        CFXLOG("orig VirtualQuery: base=%p size=%zu state=0x%X protect=0x%X type=0x%X",
               mbi.BaseAddress, (size_t)mbi.RegionSize,
               (unsigned)mbi.State, (unsigned)mbi.Protect, (unsigned)mbi.Type);
    } else {
        CFXLOG("orig VirtualQuery FAILED err=%lu — pointer may be invalid!", GetLastError());
    }
    HRESULT hr = S_OK;
    __try {
        CFXLOG("cfx_CallOrigWithSEH: calling orig=%p", cfx_OriginalPresent);
        hr = cfx_OriginalPresent(sc, sync, flags);
        CFXLOG("cfx_CallOrigWithSEH: orig returned hr=0x%08X", (unsigned)hr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ULONG_PTR stackLow = 0, stackHigh = 0;
        GetCurrentThreadStackLimits(&stackLow, &stackHigh);
        ULONG_PTR approxRSP = (ULONG_PTR)_AddressOfReturnAddress();
        CFXLOG("SEH: exception 0x%08X in orig -- RSP~=%p low=%p high=%p freeKB=%llu",
               (unsigned)GetExceptionCode(),
               (void*)approxRSP, (void*)stackLow, (void*)stackHigh,
               (unsigned long long)(approxRSP - stackLow) / 1024);
        return DXGI_ERROR_DEVICE_RESET;
    }
    CFXLOG("cfx_CallOrigWithSEH: returning hr=0x%08X", (unsigned)hr);
    return hr;
}

static void* GetPresentSlotAddress(void*** outVtableSlot)
{
    WNDCLASSEXA wc   = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcA;
    wc.lpszClassName = "chaosfx_dummy";
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowExA(0, "chaosfx_dummy", "", WS_OVERLAPPEDWINDOW,
                                0, 0, 8, 8, nullptr, nullptr, nullptr, nullptr);
    if (!hwnd) return nullptr;

    ID3D11Device* device = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                 nullptr, 0, D3D11_SDK_VERSION,
                                 &device, nullptr, nullptr))) {
        DestroyWindow(hwnd);
        return nullptr;
    }

    IDXGIDevice*   dxgiDev  = nullptr;
    IDXGIAdapter*  adapter  = nullptr;
    IDXGIFactory2* factory2 = nullptr;
    device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev));
    if (dxgiDev) dxgiDev->GetAdapter(&adapter);
    if (adapter) adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory2));

    IDXGISwapChain1* sc = nullptr;
    if (factory2) {
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width  = sd.Height = 8;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount  = 2;
        sd.SwapEffect   = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        factory2->CreateSwapChainForHwnd(device, hwnd, &sd, nullptr, nullptr, &sc);
    }

    void** slot = nullptr;
    void*  orig = nullptr;
    if (sc) {
        void** vtable = *reinterpret_cast<void***>(sc);
        slot = &vtable[PRESENT_VTABLE_SLOT];
        orig = *slot;
        sc->Release();
    }

    if (factory2) factory2->Release();
    if (adapter)  adapter->Release();
    if (dxgiDev)  dxgiDev->Release();
    device->Release();
    DestroyWindow(hwnd);

    if (outVtableSlot) *outVtableSlot = slot;
    return orig;
}

bool Install(std::function<void(IDXGISwapChain*, UINT, UINT)> onPresent)
{
    if (g_PresentSlot) return true;

    void** slot = nullptr;
    void*  orig = GetPresentSlotAddress(&slot);
    CFXLOG("Install: slot=%p orig=%p", slot, orig);
    if (!slot || !orig) return false;

    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        CFXLOG("Install: VirtualProtect failed err=%lu", GetLastError());
        return false;
    }

    cfx_OriginalPresent = reinterpret_cast<PresentFn>(orig);
    g_PresentSlot       = slot;
    *slot               = reinterpret_cast<void*>(&HookedPresentThunk);
    VirtualProtect(slot, sizeof(void*), old, &old);

    {
        std::lock_guard<std::mutex> lock(g_CallbackMutex);
        g_Callback = std::move(onPresent);
    }

    CFXLOG("Install OK: slot=%p orig=%p thunk=%p", g_PresentSlot, cfx_OriginalPresent, &HookedPresentThunk);
    return true;
}

void Uninstall()
{
    CFXLOG("Uninstall called slot=%p orig=%p", g_PresentSlot, cfx_OriginalPresent);
    {
        std::lock_guard<std::mutex> lock(g_CallbackMutex);
        g_Callback = nullptr;
    }
    if (g_PresentSlot && cfx_OriginalPresent) {
        DWORD old;
        if (VirtualProtect(g_PresentSlot, sizeof(void*), PAGE_READWRITE, &old)) {
            *g_PresentSlot = reinterpret_cast<void*>(cfx_OriginalPresent);
            VirtualProtect(g_PresentSlot, sizeof(void*), old, &old);
            CFXLOG("Uninstall: vtable restored");
        }
        g_PresentSlot       = nullptr;
        // NOTE: do NOT zero cfx_OriginalPresent here.
        // The ASM thunk's JMP to cfx_CallOrigWithSEH still needs it
        // to call orig after this Uninstall returns.
    }
    CFXLOG("Uninstall done");
}

} // namespace chaosfx::hook
