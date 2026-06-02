#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>

// ── Real d3d11.dll module, loaded from System32 ────────────────────────────
extern HMODULE g_RealD3D11;

// ── Captured D3D state ─────────────────────────────────────────────────────
extern ID3D11Device*        g_CfxDevice;
extern ID3D11DeviceContext* g_CfxContext;
extern IDXGISwapChain*      g_CfxSwapChain;
extern volatile LONG        g_CfxReady;   // 1 once swapchain has been captured

// ── Internal helpers ───────────────────────────────────────────────────────
void Proxy_HookFactory(IUnknown* pDevice);
// Hook IDXGISwapChain::Present on the captured swapchain (idempotent).
void Proxy_HookPresent(IDXGISwapChain* sc);
