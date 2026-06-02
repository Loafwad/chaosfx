Texture2D    BackBuffer : register(t0);
SamplerState LinearSampler : register(s0);

cbuffer EffectCB : register(b0) {
    float intensity;
    float time;
    float2 _pad;
};

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

float4 main(VSOut input) : SV_Target
{
    float4 c    = BackBuffer.Sample(LinearSampler, input.uv);
    float  luma = dot(c.rgb, float3(0.2126, 0.7152, 0.0722));

    // Desaturate fully, then tint pink
    float3 pink  = float3(1.0, 0.18, 0.55);
    float3 tinted = lerp(c.rgb, luma * pink, intensity);

    // Subtle animated shimmer so it reads as an active effect
    float shimmer = sin(input.uv.y * 80.0 + time * 4.0) * 0.04 * intensity;
    tinted += shimmer;

    return float4(saturate(tinted), c.a);
}
