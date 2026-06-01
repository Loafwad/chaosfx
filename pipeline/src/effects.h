#pragma once
#include "effect_types.h"
#include <d3d11.h>
#include <dxgi.h>

namespace chaosfx::effects {

// Called once on first Present (render thread): saves device/context/swapchain
// and compiles shaders + creates all D3D11 objects in one shot.
bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain);

void Shutdown();

void SetEffect(EffectType type, float intensity);
void ClearEffect();

// Called from ManiaScript @RenderOverlay every frame (before Present).
// Uses the saved swapchain from Initialize. No-ops if not Ready or no effect active.
bool RenderFrame();

} // namespace chaosfx::effects
