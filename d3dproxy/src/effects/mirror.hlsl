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
    float2 uv = input.uv;
    float2 mirroredUV = float2(uv.x > 0.5 ? 1.0 - uv.x : uv.x, uv.y);

    float4 orig     = BackBuffer.Sample(LinearSampler, uv);
    float4 mirrored = BackBuffer.Sample(LinearSampler, mirroredUV);

    return lerp(orig, mirrored, intensity);
}
