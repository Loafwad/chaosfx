#include "proxy.h"
#include "http_poll.h"
#include "log.h"
#include "proxy_drawcall.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        // Load the real d3d11.dll from System32 before anything uses ours.
        char sysDir[MAX_PATH];
        GetSystemDirectoryA(sysDir, MAX_PATH);
        strcat_s(sysDir, "\\d3d11.dll");
        g_RealD3D11 = LoadLibraryA(sysDir);
        if (!g_RealD3D11) {
            OutputDebugStringA("[ChaosFXProxy] FATAL: could not load System32\\d3d11.dll\n");
            return FALSE;
        }
        OutputDebugStringA("[ChaosFXProxy] Real d3d11.dll loaded — proxy active\n");
        chaosfx::log::begin_session();
        Proxy_StartPolling();
    }
    else if (reason == DLL_PROCESS_DETACH) {
        Proxy_UnhookDrawCalls();
    }
    return TRUE;
}
