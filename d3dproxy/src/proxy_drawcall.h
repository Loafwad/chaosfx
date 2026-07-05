#pragma once
#include <d3d11.h>
#include <atomic>
#include <cstdint>

// Skip draw call #N each frame (-1 = disabled).
// Written by the poll thread, read by Hook_DrawIndexed.
extern std::atomic<int>       g_SkipDrawN;
extern std::atomic<bool>      g_HooksInstalled;
extern std::atomic<int>       g_DrawsLastFrame;
extern std::atomic<int>       g_PresentCount;

struct DrawSkipInfo {
    unsigned int  indexCount;
    unsigned int  startIndex;
    uintptr_t     vsPtr;
    uintptr_t     psPtr;
};

struct ShaderPairEntry {
    uint64_t vsHash;
    uint64_t psHash;
    int      count;
};

// Hook all ID3D11DeviceContext draw calls via MinHook inline patching (idempotent).
void Proxy_HookDrawCalls(ID3D11DeviceContext* ctx);

// Hook CreateVertexShader + CreatePixelShader to build bytecode-hash map (idempotent).
void Proxy_HookShaderCreate(ID3D11Device* dev);

// Remove MinHook patches — call on DLL_PROCESS_DETACH.
void Proxy_UnhookDrawCalls();

// Called from Hook_Present to reset the per-frame counter.
void Proxy_ResetDrawCounter();

// Consume pending skip info (returns false if none waiting).
bool Proxy_TakeSkipInfo(DrawSkipInfo* out);

// Copy latest per-frame shader histogram snapshot. Returns count written.
int  Proxy_TakeShaderHist(ShaderPairEntry* buf, int maxEntries);

// Replace the entire set of suppressed shader pairs (up to 32). count=0 clears all.
void Proxy_SetSkipPairs(const uint64_t* vsArr, const uint64_t* psArr, int count);

// Skip draws matching any of these index/vertex counts (up to 32). count=0 clears all.
void Proxy_SetSkipIndexCounts(const unsigned int* counts, int count);

struct IndexCountEntry {
    unsigned int indexCount;
    int          drawCount;
};

// Copy latest per-frame index-count histogram snapshot. Returns count written.
int Proxy_TakeIndexCountHist(IndexCountEntry* buf, int maxEntries);
