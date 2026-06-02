#pragma once
#include <cstdint>

namespace chaosfx {

enum class EffectType : int32_t {
    None         = 0,
    PinkMode     = 1,
    Kaleidoscope = 2,
    Mirror       = 3,
    FlippedScreen = 4,
};

struct EffectState {
    EffectType active    = EffectType::None;
    float      progress  = 0.0f;
    float      intensity = 1.0f;
};

} // namespace chaosfx
