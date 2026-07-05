#include "proxy.h"
#include "proxy_drawcall.h"
#include "log.h"
#include "minhook/MinHook.h"
#include <d3d11.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>
#include <windows.h>

// ── Shared skip-N state (written by poll thread, read by draw hooks) ────────
std::atomic<int>  g_SkipDrawN      { -1 };
std::atomic<bool> g_HooksInstalled { false };

// ── Per-frame draw counter (reset each Present) ────────────────────────────
static std::atomic<int> g_DrawCounter { 0 };
std::atomic<int>        g_DrawsLastFrame { 0 };
std::atomic<int>        g_PresentCount   { 0 };

// ── Last-skip info (written by draw hook, consumed by poll thread) ──────────
static std::mutex        g_SkipInfoMutex;
static DrawSkipInfo      g_SkipInfoBuf  {};
static std::atomic<bool> g_SkipInfoPending { false };

// ── Skip-by-shader-pair set (written by poll thread, read by DrawTick) ──────
static const int  kMaxSkipPairs = 32;
static std::mutex g_SkipSetMutex;
static uint64_t   g_SkipSetVS[kMaxSkipPairs];
static uint64_t   g_SkipSetPS[kMaxSkipPairs];
static int        g_SkipSetCount = 0;

// ── Skip-by-index-count set (written by poll thread, read by DrawTick) ──────
static const int      kMaxSkipIndexCounts = 32;
static std::mutex     g_SkipIndexCountMutex;
static unsigned int   g_SkipIndexCounts[kMaxSkipIndexCounts];
static int            g_SkipIndexCountCount = 0;

// ── Shader bytecode hash map (ptr → FNV-1a hash, set at CreateShader time) ─
static std::mutex                           g_ShaderHashMutex;
static std::unordered_map<uintptr_t,uint64_t> g_VSHashMap;
static std::unordered_map<uintptr_t,uint64_t> g_PSHashMap;

static uint64_t Fnv1a64(const void* data, size_t len)
{
    uint64_t h = 14695981039346656037ull;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// ── Current VS/PS (render thread only, no lock needed) ──────────────────────
static uintptr_t s_CurVS = 0;
static uintptr_t s_CurPS = 0;
static uint64_t  s_CurVSHash = 0;
static uint64_t  s_CurPSHash = 0;

// ── Per-frame shader histogram ─────────────────────────────────────────────
struct PairHash {
    size_t operator()(std::pair<uint64_t,uint64_t> const& p) const noexcept {
        return std::hash<uint64_t>()(p.first) ^ (std::hash<uint64_t>()(p.second) * 2654435761ull);
    }
};
static std::unordered_map<std::pair<uint64_t,uint64_t>, int, PairHash> s_HistAcc;
static std::mutex                   s_HistMutex;
static std::vector<ShaderPairEntry> s_HistSnapshot;

// ── Per-frame index-count histogram ────────────────────────────────────────
static std::mutex                          s_IdxHistMutex;
static std::unordered_map<unsigned int,int> s_IdxHistAcc;
static std::vector<IndexCountEntry>        s_IdxHistSnapshot;

typedef void (STDMETHODCALLTYPE* PFN_DrawIndexed)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void (STDMETHODCALLTYPE* PFN_Draw)(ID3D11DeviceContext*, UINT, UINT);
typedef void (STDMETHODCALLTYPE* PFN_DrawIndexedInstanced)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef void (STDMETHODCALLTYPE* PFN_DrawInstanced)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
typedef void (STDMETHODCALLTYPE* PFN_DrawAuto)(ID3D11DeviceContext*);
typedef void (STDMETHODCALLTYPE* PFN_DrawIndexedInstancedIndirect)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
typedef void (STDMETHODCALLTYPE* PFN_DrawInstancedIndirect)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);

static PFN_DrawIndexed                  g_OrigDrawIndexed                  = nullptr;
static PFN_Draw                         g_OrigDraw                         = nullptr;
static PFN_DrawIndexedInstanced         g_OrigDrawIndexedInstanced         = nullptr;
static PFN_DrawInstanced                g_OrigDrawInstanced                = nullptr;
static PFN_DrawAuto                     g_OrigDrawAuto                     = nullptr;
static PFN_DrawIndexedInstancedIndirect g_OrigDrawIndexedInstancedIndirect = nullptr;
static PFN_DrawInstancedIndirect        g_OrigDrawInstancedIndirect        = nullptr;

typedef void (STDMETHODCALLTYPE* PFN_VSSetShader)(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT);
typedef void (STDMETHODCALLTYPE* PFN_PSSetShader)(ID3D11DeviceContext*, ID3D11PixelShader*,  ID3D11ClassInstance* const*, UINT);
static PFN_VSSetShader g_OrigVSSetShader = nullptr;
static PFN_PSSetShader g_OrigPSSetShader = nullptr;

typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateVertexShader)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreatePixelShader) (ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
static PFN_CreateVertexShader g_OrigCreateVertexShader = nullptr;
static PFN_CreatePixelShader  g_OrigCreatePixelShader  = nullptr;

static HRESULT STDMETHODCALLTYPE Hook_CreateVertexShader(
    ID3D11Device* dev, const void* bc, SIZE_T sz, ID3D11ClassLinkage* lnk, ID3D11VertexShader** out)
{
    HRESULT hr = g_OrigCreateVertexShader(dev, bc, sz, lnk, out);
    if (SUCCEEDED(hr) && out && *out) {
        uint64_t h = Fnv1a64(bc, sz);
        std::lock_guard<std::mutex> lk(g_ShaderHashMutex);
        g_VSHashMap[reinterpret_cast<uintptr_t>(*out)] = h;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreatePixelShader(
    ID3D11Device* dev, const void* bc, SIZE_T sz, ID3D11ClassLinkage* lnk, ID3D11PixelShader** out)
{
    HRESULT hr = g_OrigCreatePixelShader(dev, bc, sz, lnk, out);
    if (SUCCEEDED(hr) && out && *out) {
        uint64_t h = Fnv1a64(bc, sz);
        std::lock_guard<std::mutex> lk(g_ShaderHashMutex);
        g_PSHashMap[reinterpret_cast<uintptr_t>(*out)] = h;
    }
    return hr;
}

static void STDMETHODCALLTYPE Hook_VSSetShader(
    ID3D11DeviceContext* ctx, ID3D11VertexShader* vs, ID3D11ClassInstance* const* ci, UINT n)
{
    s_CurVS = reinterpret_cast<uintptr_t>(vs);
    {
        std::lock_guard<std::mutex> lk(g_ShaderHashMutex);
        auto it = g_VSHashMap.find(s_CurVS);
        s_CurVSHash = (it != g_VSHashMap.end()) ? it->second : 0;
    }
    g_OrigVSSetShader(ctx, vs, ci, n);
}
static void STDMETHODCALLTYPE Hook_PSSetShader(
    ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11ClassInstance* const* ci, UINT n)
{
    s_CurPS = reinterpret_cast<uintptr_t>(ps);
    {
        std::lock_guard<std::mutex> lk(g_ShaderHashMutex);
        auto it = g_PSHashMap.find(s_CurPS);
        s_CurPSHash = (it != g_PSHashMap.end()) ? it->second : 0;
    }
    g_OrigPSSetShader(ctx, ps, ci, n);
}

// Accumulate histograms + check shader-pair skip and index-count skip.
// count = IndexCount/VertexCount from the draw call (0 for methods without a direct count).
// Returns true = suppress this draw.
static bool DrawTick(UINT count = 0)
{
    s_HistAcc[{s_CurVSHash, s_CurPSHash}]++;

    if (count > 0) {
        std::lock_guard<std::mutex> lk(s_IdxHistMutex);
        s_IdxHistAcc[count]++;
    }

    // Check shader-pair skip
    {
        std::lock_guard<std::mutex> lk(g_SkipSetMutex);
        for (int i = 0; i < g_SkipSetCount; i++)
            if (s_CurVSHash == g_SkipSetVS[i] && s_CurPSHash == g_SkipSetPS[i])
                return true;
    }

    // Check index-count skip
    {
        std::lock_guard<std::mutex> lk(g_SkipIndexCountMutex);
        for (int i = 0; i < g_SkipIndexCountCount; i++)
            if (count == g_SkipIndexCounts[i])
                return true;
    }

    return false;
}

// Record skip info for the poll thread (uses s_CurVS/s_CurPS, no GetShader call).
static void CaptureAndMark(UINT indexCount, UINT startIndex)
{
    std::lock_guard<std::mutex> lk(g_SkipInfoMutex);
    g_SkipInfoBuf = { indexCount, startIndex, s_CurVS, s_CurPS };
    g_SkipInfoPending.store(true, std::memory_order_release);
}

static void STDMETHODCALLTYPE Hook_DrawIndexed(
    ID3D11DeviceContext* ctx,
    UINT  IndexCount,
    UINT  StartIndexLocation,
    INT   BaseVertexLocation)
{
    if (DrawTick(IndexCount)) return;
    int n = g_SkipDrawN.load(std::memory_order_relaxed);
    int c = g_DrawCounter.fetch_add(1, std::memory_order_relaxed);
    if (n >= 0 && c >= n) { if (c == n) CaptureAndMark(IndexCount, StartIndexLocation); return; }
    g_OrigDrawIndexed(ctx, IndexCount, StartIndexLocation, BaseVertexLocation);
}

static void STDMETHODCALLTYPE Hook_Draw(
    ID3D11DeviceContext* ctx,
    UINT VertexCount,
    UINT StartVertexLocation)
{
    if (DrawTick(VertexCount)) return;
    int n = g_SkipDrawN.load(std::memory_order_relaxed);
    int c = g_DrawCounter.fetch_add(1, std::memory_order_relaxed);
    if (n >= 0 && c >= n) { if (c == n) CaptureAndMark(VertexCount, StartVertexLocation); return; }
    g_OrigDraw(ctx, VertexCount, StartVertexLocation);
}

static void STDMETHODCALLTYPE Hook_DrawIndexedInstanced(
    ID3D11DeviceContext* ctx,
    UINT IndexCountPerInstance, UINT InstanceCount,
    UINT StartIndexLocation, INT  BaseVertexLocation, UINT StartInstanceLocation)
{
    if (DrawTick(IndexCountPerInstance)) return;
    int n = g_SkipDrawN.load(std::memory_order_relaxed);
    int c = g_DrawCounter.fetch_add(1, std::memory_order_relaxed);
    if (n >= 0 && c >= n) { if (c == n) CaptureAndMark(IndexCountPerInstance, StartIndexLocation); return; }
    g_OrigDrawIndexedInstanced(ctx, IndexCountPerInstance, InstanceCount,
                               StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
}

static void STDMETHODCALLTYPE Hook_DrawInstanced(
    ID3D11DeviceContext* ctx,
    UINT VertexCountPerInstance, UINT InstanceCount,
    UINT StartVertexLocation,    UINT StartInstanceLocation)
{
    if (DrawTick(VertexCountPerInstance)) return;
    int n = g_SkipDrawN.load(std::memory_order_relaxed);
    int c = g_DrawCounter.fetch_add(1, std::memory_order_relaxed);
    if (n >= 0 && c >= n) { if (c == n) CaptureAndMark(VertexCountPerInstance, StartVertexLocation); return; }
    g_OrigDrawInstanced(ctx, VertexCountPerInstance, InstanceCount,
                        StartVertexLocation, StartInstanceLocation);
}

static void STDMETHODCALLTYPE Hook_DrawAuto(ID3D11DeviceContext* ctx)
{
    if (DrawTick()) return;
    int n = g_SkipDrawN.load(std::memory_order_relaxed);
    int c = g_DrawCounter.fetch_add(1, std::memory_order_relaxed);
    if (n >= 0 && c >= n) { if (c == n) CaptureAndMark(0, 0); return; }
    g_OrigDrawAuto(ctx);
}

static void STDMETHODCALLTYPE Hook_DrawIndexedInstancedIndirect(
    ID3D11DeviceContext* ctx, ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs)
{
    if (DrawTick()) return;
    int n = g_SkipDrawN.load(std::memory_order_relaxed);
    int c = g_DrawCounter.fetch_add(1, std::memory_order_relaxed);
    if (n >= 0 && c >= n) { if (c == n) CaptureAndMark(0, 0); return; }
    g_OrigDrawIndexedInstancedIndirect(ctx, pBufferForArgs, AlignedByteOffsetForArgs);
}

static void STDMETHODCALLTYPE Hook_DrawInstancedIndirect(
    ID3D11DeviceContext* ctx, ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs)
{
    if (DrawTick()) return;
    int n = g_SkipDrawN.load(std::memory_order_relaxed);
    int c = g_DrawCounter.fetch_add(1, std::memory_order_relaxed);
    if (n >= 0 && c >= n) { if (c == n) CaptureAndMark(0, 0); return; }
    g_OrigDrawInstancedIndirect(ctx, pBufferForArgs, AlignedByteOffsetForArgs);
}

// ── Hook setup via MinHook (inline code patching, bypasses vtable wrappers) ─
void Proxy_HookDrawCalls(ID3D11DeviceContext* ctx)
{
    if (g_HooksInstalled.load(std::memory_order_relaxed)) return;

    void** vt = *reinterpret_cast<void***>(ctx);
    CFXLOG("Proxy_HookDrawCalls: ctx=%p vtable=%p slot12=%p slot13=%p",
        ctx, vt, vt[12], vt[13]);

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        CFXLOG("Proxy_HookDrawCalls: MH_Initialize failed: %d", (int)st);
        return;
    }

    struct HookEntry { int slot; void* detour; void** orig; };
    HookEntry hooks[] = {
        {  9, (void*)&Hook_PSSetShader,                  (void**)&g_OrigPSSetShader },
        { 11, (void*)&Hook_VSSetShader,                  (void**)&g_OrigVSSetShader },
        { 12, (void*)&Hook_DrawIndexed,                  (void**)&g_OrigDrawIndexed },
        { 13, (void*)&Hook_Draw,                         (void**)&g_OrigDraw },
        { 20, (void*)&Hook_DrawIndexedInstanced,         (void**)&g_OrigDrawIndexedInstanced },
        { 21, (void*)&Hook_DrawInstanced,                (void**)&g_OrigDrawInstanced },
        { 38, (void*)&Hook_DrawAuto,                     (void**)&g_OrigDrawAuto },
        { 39, (void*)&Hook_DrawIndexedInstancedIndirect, (void**)&g_OrigDrawIndexedInstancedIndirect },
        { 40, (void*)&Hook_DrawInstancedIndirect,        (void**)&g_OrigDrawInstancedIndirect },
    };
    for (auto& e : hooks) {
        void* target = vt[e.slot];
        MH_STATUS s = MH_CreateHook(target, e.detour, e.orig);
        CFXLOG("Proxy_HookDrawCalls: CreateHook slot%d target=%p status=%d", e.slot, target, (int)s);
    }

    st = MH_EnableHook(MH_ALL_HOOKS);
    CFXLOG("Proxy_HookDrawCalls: MH_EnableHook status=%d", (int)st);

    g_HooksInstalled.store(true, std::memory_order_release);
}

void Proxy_HookShaderCreate(ID3D11Device* dev)
{
    static bool s_Done = false;
    if (s_Done) return;
    s_Done = true;

    void** vt = *reinterpret_cast<void***>(dev);

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return;

    struct E { int slot; void* detour; void** orig; };
    E hooks[] = {
        { 12, (void*)&Hook_CreateVertexShader, (void**)&g_OrigCreateVertexShader },
        { 15, (void*)&Hook_CreatePixelShader,  (void**)&g_OrigCreatePixelShader  },
    };
    for (auto& e : hooks)
        MH_CreateHook(vt[e.slot], e.detour, e.orig);
    MH_EnableHook(MH_ALL_HOOKS);
    CFXLOG("Proxy_HookShaderCreate: hooked CreateVertexShader + CreatePixelShader");
}

void Proxy_UnhookDrawCalls()
{
    if (!g_HooksInstalled.load(std::memory_order_relaxed)) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_HooksInstalled.store(false, std::memory_order_release);
}

void Proxy_ResetDrawCounter()
{
    int prev = g_DrawCounter.exchange(0, std::memory_order_relaxed);
    g_DrawsLastFrame.store(prev, std::memory_order_relaxed);
    // Snapshot shader-pair histogram
    {
        std::lock_guard<std::mutex> lk(s_HistMutex);
        s_HistSnapshot.clear();
        s_HistSnapshot.reserve(s_HistAcc.size());
        for (auto& [k, v] : s_HistAcc)
            s_HistSnapshot.push_back({ k.first, k.second, v });
        s_HistAcc.clear();
    }
    // Snapshot index-count histogram
    {
        std::lock_guard<std::mutex> lk(s_IdxHistMutex);
        s_IdxHistSnapshot.clear();
        s_IdxHistSnapshot.reserve(s_IdxHistAcc.size());
        for (auto& [c, n] : s_IdxHistAcc)
            s_IdxHistSnapshot.push_back({ c, n });
        s_IdxHistAcc.clear();
    }
}

int Proxy_TakeShaderHist(ShaderPairEntry* buf, int maxEntries)
{
    std::lock_guard<std::mutex> lk(s_HistMutex);
    int n = (int)s_HistSnapshot.size();
    if (n > maxEntries) n = maxEntries;
    for (int i = 0; i < n; ++i) buf[i] = s_HistSnapshot[i];
    return n;
}

bool Proxy_TakeSkipInfo(DrawSkipInfo* out)
{
    if (!g_SkipInfoPending.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lk(g_SkipInfoMutex);
    *out = g_SkipInfoBuf;
    g_SkipInfoPending.store(false, std::memory_order_release);
    return true;
}

void Proxy_SetSkipPairs(const uint64_t* vsArr, const uint64_t* psArr, int count)
{
    if (count > kMaxSkipPairs) count = kMaxSkipPairs;
    std::lock_guard<std::mutex> lk(g_SkipSetMutex);
    for (int i = 0; i < count; i++) { g_SkipSetVS[i] = vsArr[i]; g_SkipSetPS[i] = psArr[i]; }
    g_SkipSetCount = count;
}

void Proxy_SetSkipIndexCounts(const unsigned int* counts, int count)
{
    if (count > kMaxSkipIndexCounts) count = kMaxSkipIndexCounts;
    std::lock_guard<std::mutex> lk(g_SkipIndexCountMutex);
    for (int i = 0; i < count; i++) g_SkipIndexCounts[i] = counts[i];
    g_SkipIndexCountCount = count;
}

int Proxy_TakeIndexCountHist(IndexCountEntry* buf, int maxEntries)
{
    std::lock_guard<std::mutex> lk(s_IdxHistMutex);
    int n = (int)s_IdxHistSnapshot.size();
    if (n > maxEntries) n = maxEntries;
    for (int i = 0; i < n; ++i) buf[i] = s_IdxHistSnapshot[i];
    return n;
}
