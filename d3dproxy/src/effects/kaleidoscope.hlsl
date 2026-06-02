Texture2D    BackBuffer : register(t0);
SamplerState LinearSampler : register(s0);

cbuffer EffectCB : register(b0) {
    float intensity;
    float time;
    float2 _pad;
};

#define PI 3.14159265

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

float4 main(VSOut input) : SV_Target
{
    float4 original = BackBuffer.Sample(LinearSampler, input.uv);

    float2 centered = input.uv - float2(0.5, 0.5);
    float  r        = length(centered);

    // Inner safe zone — never distorted
    float innerR = 0.10;
    // Outer edge where effect is fully applied
    float outerR = 0.38;

    // If inside the safe zone, return original unmodified
    if (r < innerR) return original;

    float theta = atan2(centered.y, centered.x);

    // 8 segments = 45° each
    float segments  = 8.0;
    float angleStep = PI * 2.0 / segments;
    float rotated   = theta + time * 0.25;
    float folded    = fmod(abs(rotated), angleStep);
    if (folded > angleStep * 0.5) folded = angleStep - folded;

    float2 foldedUV = float2(cos(folded), sin(folded)) * r + float2(0.5, 0.5);
    float4 kaleido  = BackBuffer.Sample(LinearSampler, foldedUV);

    float radialBlend = saturate((r - innerR) / (outerR - innerR));
    float blend = radialBlend * intensity;
    return lerp(original, kaleido, blend);
}
