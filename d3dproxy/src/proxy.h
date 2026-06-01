#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>

// ── Real d3d11.dll module, loaded from System32 ────────────────────────────
extern HMODULE g_RealD3D11;

// ── Captured D3D state ─────────────────────────────────────────────────────
// All three pointers are held alive with AddRef for the lifetime of the process.
// ChaosFXPipeline.dll reads these via the cfx_Get* exports (raw pointers, no AddRef).
extern ID3D11Device*        g_CfxDevice;
extern ID3D11DeviceContext* g_CfxContext;
extern IDXGISwapChain*      g_CfxSwapChain;
extern volatile LONG        g_CfxReady;   // 1 once swapchain has been captured

// ── Called by ChaosFXPipeline.dll (via GetProcAddress on "d3d11.dll") ──────
extern "C" {
    __declspec(dllexport) ID3D11Device*        cfx_GetDevice();
    __declspec(dllexport) ID3D11DeviceContext* cfx_GetContext();
    __declspec(dllexport) IDXGISwapChain*      cfx_GetSwapChain();
    __declspec(dllexport) BOOL                 cfx_IsReady();
}

// ── Internal: called from dllmain to wire up the factory vtable hook ───────
void Proxy_HookFactory(IUnknown* pDevice);
