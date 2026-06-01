#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <functional>

namespace chaosfx::hook {

// Install a MinHook inline hook on IDXGISwapChain::Present.
// A dummy swapchain is created to locate the function; MinHook creates a
// proper trampoline so calling orig never re-reads the vtable.
bool Install(std::function<void(IDXGISwapChain*, UINT, UINT)> onPresent);
void Uninstall();

} // namespace chaosfx::hook
