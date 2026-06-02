#include "proxy.h"

HMODULE              g_RealD3D11    = nullptr;
ID3D11Device*        g_CfxDevice    = nullptr;
ID3D11DeviceContext* g_CfxContext   = nullptr;
IDXGISwapChain*      g_CfxSwapChain = nullptr;
volatile LONG        g_CfxReady     = 0;
