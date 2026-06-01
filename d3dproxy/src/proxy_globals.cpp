#include "proxy.h"

HMODULE              g_RealD3D11    = nullptr;
ID3D11Device*        g_CfxDevice    = nullptr;
ID3D11DeviceContext* g_CfxContext   = nullptr;
IDXGISwapChain*      g_CfxSwapChain = nullptr;
volatile LONG        g_CfxReady     = 0;

extern "C" {
    __declspec(dllexport) ID3D11Device*        cfx_GetDevice()    { return g_CfxDevice; }
    __declspec(dllexport) ID3D11DeviceContext* cfx_GetContext()   { return g_CfxContext; }
    __declspec(dllexport) IDXGISwapChain*      cfx_GetSwapChain() { return g_CfxSwapChain; }
    __declspec(dllexport) BOOL                 cfx_IsReady()      { return g_CfxReady != 0; }
}
