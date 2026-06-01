#pragma once
#include <cstdint>

namespace chaosfx {

enum class EffectType : int32_t {
    None         = 0,
    PinkMode     = 1,
    Kaleidoscope = 2,
    Mirror       = 3,
};

struct EffectState {
    EffectType active   = EffectType::None;
    float      progress = 0.0f;  // 0..1, driven by elapsed/duration
    float      intensity = 1.0f; // configurable per-effect
};

} // namespace chaosfx
