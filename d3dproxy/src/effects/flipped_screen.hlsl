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
    float2 flippedUV = float2(1.0 - input.uv.x, input.uv.y);
    float4 orig    = BackBuffer.Sample(LinearSampler, input.uv);
    float4 flipped = BackBuffer.Sample(LinearSampler, flippedUV);
    return lerp(orig, flipped, intensity);
}
