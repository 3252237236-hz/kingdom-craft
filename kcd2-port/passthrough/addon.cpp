#include "compositor.h"

#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        return compositor::try_register(module) ? TRUE : FALSE;
    }
    if (reason == DLL_PROCESS_DETACH) {
        compositor::unregister(module);
    }
    return TRUE;
}
