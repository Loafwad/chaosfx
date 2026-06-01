#include "effects.h"
#include "log.h"
#include <windows.h>

// Vectored Exception Handler — fires before SEH, including for __fastfail / non-continuable.
// This is our only chance to log what exception is killing the process.
static LONG NTAPI CfxVehHandler(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    DWORD flags = ep->ExceptionRecord->ExceptionFlags;
    chaosfx::log::write("[VEH] tid=%lu code=0x%08X addr=%p flags=0x%X noncontinuable=%d",
                        GetCurrentThreadId(),
                        (unsigned)code, addr, (unsigned)flags,
                        (flags & EXCEPTION_NONCONTINUABLE) ? 1 : 0);
    return EXCEPTION_CONTINUE_SEARCH; // never handle, only log
}

// Called once when Openplanet loads/unloads the DLL
BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        chaosfx::log::begin_session();
        AddVectoredExceptionHandler(1, CfxVehHandler); // 1 = prepend, called first
    }
    if (reason == DLL_PROCESS_DETACH) {
        CFXLOG("DLL_PROCESS_DETACH: calling Shutdown");
        chaosfx::effects::Shutdown();
        CFXLOG("DLL_PROCESS_DETACH done");
    }
    return TRUE;
}
