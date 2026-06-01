#include "effects.h"
#include "log.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

extern "C" {

__declspec(dllexport)
void InitializeDX11Hook()
{
    CFXLOG("InitializeDX11Hook called");

    // The ChaosFX d3d11.dll proxy is already loaded in the game process —
    // it intercepts D3D11CreateDevice and IDXGIFactory::CreateSwapChain* to
    // capture the device/context/swapchain before Openplanet touches anything.
    // We just call GetProcAddress on the already-loaded module to retrieve them.
    HMODULE hProxy = GetModuleHandleA("d3d11.dll");
    if (!hProxy) {
        CFXLOG("InitializeDX11Hook: d3d11.dll not in process — proxy not installed");
        return;
    }

    auto fnIsReady   = reinterpret_cast<BOOL(*)()>            (GetProcAddress(hProxy, "cfx_IsReady"));
    auto fnGetDevice = reinterpret_cast<ID3D11Device*(*)()>   (GetProcAddress(hProxy, "cfx_GetDevice"));
    auto fnGetCtx    = reinterpret_cast<ID3D11DeviceContext*(*)()>(GetProcAddress(hProxy, "cfx_GetContext"));
    auto fnGetSC     = reinterpret_cast<IDXGISwapChain*(*)()> (GetProcAddress(hProxy, "cfx_GetSwapChain"));

    if (!fnIsReady || !fnGetDevice || !fnGetCtx || !fnGetSC) {
        CFXLOG("InitializeDX11Hook: cfx_* exports not found — wrong d3d11.dll in game dir?");
        return;
    }
    CFXLOG("InitializeDX11Hook: proxy exports found, launching init thread");

    // Capture the function pointers into a heap struct and spin up an 8MB worker.
    // D3DCompile's recursive HLSL parser needs well over 1MB of stack.
    struct Args {
        BOOL(*isReady)();
        ID3D11Device*(*getDevice)();
        ID3D11DeviceContext*(*getCtx)();
        IDXGISwapChain*(*getSC)();
    };
    auto* args = new Args{ fnIsReady, fnGetDevice, fnGetCtx, fnGetSC };

    HANDLE h = CreateThread(nullptr, 8 * 1024 * 1024,
        [](LPVOID param) -> DWORD {
            auto* a = static_cast<Args*>(param);

            // Poll until the proxy has captured the swapchain.
            // By the time the plugin loads the game is already rendering,
            // so this usually succeeds on the very first check.
            for (int i = 0; i < 500 && !a->isReady(); ++i)
                Sleep(10);

            ID3D11Device*        device = a->getDevice();
            ID3D11DeviceContext* ctx    = a->getCtx();
            IDXGISwapChain*      sc     = a->getSC();
            delete a;

            if (!device || !ctx || !sc) {
                CFXLOG("InitializeDX11Hook: proxy not ready after 5s — aborting");
                return 1;
            }
            chaosfx::effects::Initialize(device, ctx, sc);
            return 0;
        },
        args, 0, nullptr);

    if (h) CloseHandle(h);
    else   delete args;
}

__declspec(dllexport)
void ReleaseDX11Hook()
{
    CFXLOG("ReleaseDX11Hook called");
    chaosfx::effects::Shutdown();
    CFXLOG("ReleaseDX11Hook done");
}

__declspec(dllexport)
void RenderEffect()
{
    chaosfx::effects::RenderFrame();
}

__declspec(dllexport)
void SetEffect(int effect, float intensity)
{
    chaosfx::effects::SetEffect(static_cast<chaosfx::EffectType>(effect), intensity);
}

__declspec(dllexport)
void ClearEffect()
{
    chaosfx::effects::ClearEffect();
}

} // extern "C"

