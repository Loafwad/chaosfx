// ChaosFX — Openplanet AngelScript bridge
// Calls ChaosFXPipeline.dll via Import::GetLibrary / Import::Function.

namespace ChaosFX {

    // Native DLL function handles — loaded at Initialize()
    Import::Library@ g_Lib         = null;
    Import::Function@ g_fnInit     = null;
    Import::Function@ g_fnRelease  = null;
    Import::Function@ g_fnSet      = null;
    Import::Function@ g_fnClear    = null;
    Import::Function@ g_fnRender   = null;

    bool TryLoadDll() {
        string dllPath = IO::FromDataFolder("Plugins/ChaosFX/ChaosFXPipeline.dll");
        if (!IO::FileExists(dllPath)) {
            trace("[ChaosFX] DLL not found at: " + dllPath);
            return false;
        }
        @g_Lib = Import::GetLibrary(dllPath);
        if (g_Lib is null) {
            trace("[ChaosFX] Import::GetLibrary returned null");
            return false;
        }
        @g_fnInit    = g_Lib.GetFunction("InitializeDX11Hook");
        @g_fnRelease = g_Lib.GetFunction("ReleaseDX11Hook");
        @g_fnSet     = g_Lib.GetFunction("SetEffect");
        @g_fnClear   = g_Lib.GetFunction("ClearEffect");
        @g_fnRender  = g_Lib.GetFunction("RenderEffect");
        if (g_fnInit is null || g_fnRelease is null || g_fnSet is null || g_fnClear is null || g_fnRender is null) {
            trace("[ChaosFX] Failed to resolve one or more DLL exports");
            @g_Lib = null;
            return false;
        }
        try {
            g_fnInit.Call();
        } catch {
            trace("[ChaosFX] InitializeDX11Hook threw: " + getExceptionInfo());
            @g_Lib = null;
            return false;
        }
        return true;
    }


    enum EffectType {
        None         = 0,
        PinkMode     = 1,
        Kaleidoscope = 2,
        Mirror       = 3
    }

    class RewardEvent {
        string rewardKey;
        uint durationMs;
        string source;

        RewardEvent(const string &in key, uint duration, const string &in from) {
            rewardKey = key;
            durationMs = duration;
            source = from;
        }
    }

    bool g_Initialized = false;
    bool g_DllLoaded    = false;
    EffectType g_ActiveEffect = EffectType::None;
    uint64 g_EffectEndsAt = 0;
    string g_ActiveSource = "";
    string g_Status = "idle";
    uint64 g_LastPollAt = 0;
    uint64 g_LastLogAt = 0;

    array<RewardEvent@> g_Queue;

    EffectType EffectFromKey(const string &in key) {
        string k = key.ToLower();
        if (k == "pink_mode") return EffectType::PinkMode;
        if (k == "kaleidoscope") return EffectType::Kaleidoscope;
        if (k == "mirrored_screen") return EffectType::Mirror;
        return EffectType::None;
    }

    string EffectLabel(EffectType effect) {
        if (effect == EffectType::PinkMode) return "Pink Mode";
        if (effect == EffectType::Kaleidoscope) return "Kaleidoscope";
        if (effect == EffectType::Mirror) return "Mirror";
        return "None";
    }

    void Initialize() {
        if (g_Initialized) return;
        g_Initialized = true;
        g_DllLoaded = TryLoadDll();
        if (g_DllLoaded) {
            g_Status = "hook installed";
            trace("[ChaosFX] DX11 hook installed");
        } else {
            g_Status = "no DLL - script only";
            trace("[ChaosFX] running without render hook");
        }
        startnew(CoroutineFunc(PollBridgeLoop));
    }

    void Shutdown() {
        if (!g_Initialized) return;
        g_Initialized = false;
        if (g_DllLoaded && g_fnClear !is null) {
            try { g_fnClear.Call(); } catch {}
            try { g_fnRelease.Call(); } catch {}
        }
        @g_Lib = null;
        @g_fnInit = null; @g_fnRelease = null; @g_fnSet = null; @g_fnClear = null; @g_fnRender = null;
        g_Queue.Resize(0);
        g_ActiveEffect = EffectType::None;
        g_EffectEndsAt = 0;
        g_Status = "shutdown";
        trace("[ChaosFX] shutdown");
    }

    void EnqueueReward(const string &in key, uint durationMs, const string &in source) {
        g_Queue.InsertLast(RewardEvent(key, durationMs, source));
    }

    void Tick() {
        uint64 now = Time::Now;

        if (g_Queue.Length > 0 && g_ActiveEffect == EffectType::None) {
            auto next = g_Queue[0];
            g_Queue.RemoveAt(0);
            Activate(next.rewardKey, next.durationMs, next.source);
        }

        if (g_ActiveEffect != EffectType::None && now >= g_EffectEndsAt) {
            trace("[ChaosFX] effect ended: " + EffectLabel(g_ActiveEffect));
            g_ActiveEffect = EffectType::None;
            g_EffectEndsAt = 0;
            g_ActiveSource = "";
            g_Status = "idle";
            if (g_DllLoaded && g_fnClear !is null) { try { g_fnClear.Call(); } catch {} }
        }
    }

    void Activate(const string &in rewardKey, uint durationMs, const string &in source) {
        auto effect = EffectFromKey(rewardKey);
        if (effect == EffectType::None) {
            trace("[ChaosFX] unsupported reward key: " + rewardKey);
            return;
        }

        g_ActiveEffect = effect;
        g_ActiveSource = source;
        g_EffectEndsAt = Time::Now + durationMs;
        g_Status = "active: " + EffectLabel(effect);
        if (g_DllLoaded && g_fnSet !is null) {
            try { g_fnSet.Call(int(effect), S_EffectIntensity); } catch { g_DllLoaded = false; }
        }
        trace("[ChaosFX] activated " + EffectLabel(effect) + " from " + source);
    }

    void PollBridgeLoop() {
        while (g_Initialized) {
            if (S_EnableBridgePolling) {
                PollBridgeOnce();
            }
            sleep(1000);
        }
    }

    void PollBridgeOnce() {
        uint64 now = Time::Now;
        if (now - g_LastPollAt < 900) return;
        g_LastPollAt = now;

        string url = S_BridgeBaseUrl + "/next";
        Net::HttpRequest@ req = Net::HttpGet(url);
        req.Start();

        while (!req.Finished()) {
            yield();
            if (!g_Initialized) return;
        }

        if (req.ResponseCode() == 204) return;
        if (req.ResponseCode() != 200) {
            g_Status = "bridge " + tostring(req.ResponseCode());
            return;
        }

        Json::Value payload = Json::Parse(req.String());
        if (payload.GetType() != Json::Type::Object) {
            g_Status = "bridge invalid payload";
            return;
        }

        string rewardKey = string(payload.Get("rewardKey", ""));
        if (rewardKey.Length == 0) {
            g_Status = "bridge missing rewardKey";
            return;
        }

        uint durationMs = uint(payload.Get("durationMs", 15000));
        string source = string(payload.Get("source", "twitch"));
        EnqueueReward(rewardKey, durationMs, source);
        g_Status = "queued " + rewardKey;
    }

}
// Effects are rendered by ChaosFXPipeline.dll inside the Present hook.

[Setting name="Show ChaosFX Window"]
bool S_ShowOverlay = true;

[Setting name="Enable Bridge Polling"]
bool S_EnableBridgePolling = true;

[Setting name="Bridge Base URL"]
string S_BridgeBaseUrl = "http://127.0.0.1:18244/rewards";

[Setting name="Default Duration (ms)"]
uint S_DefaultDurationMs = 15000;

[Setting name="Effect Intensity" min="0.0" max="1.0"]
float S_EffectIntensity = 1.0;

void Main() {
    ChaosFX::Initialize();
    while (ChaosFX::g_Initialized) {
        ChaosFX::Tick();
        yield();
    }
}

void OnDestroyed() {
    ChaosFX::Shutdown();
}

void RenderOverlay() {
    if (ChaosFX::g_DllLoaded && ChaosFX::g_fnRender !is null) {
        // Apply the active pixel shader effect via D3D11 before Present is called.
        try { ChaosFX::g_fnRender.Call(); } catch {}
        return;
    }
    if (ChaosFX::g_ActiveEffect == ChaosFX::EffectType::None) return;

    float W = float(Display::GetWidth());
    float H = float(Display::GetHeight());
    float t = float(Time::Now % 1000000) / 1000.0f;
    float progress = 1.0f;
    if (ChaosFX::g_EffectEndsAt > Time::Now) {
        uint64 dur = ChaosFX::g_EffectEndsAt - (Time::Now - uint64(S_DefaultDurationMs));
        if (dur > 0) progress = Math::Clamp(float(ChaosFX::g_EffectEndsAt - Time::Now) / float(S_DefaultDurationMs), 0.0f, 1.0f);
    }

    nvg::BeginPath();

    if (ChaosFX::g_ActiveEffect == ChaosFX::EffectType::PinkMode) {
        // Full screen pink wash
        float alpha = (Math::Sin(t * 2.5f) * 0.12f + 0.55f) * S_EffectIntensity;
        nvg::Rect(0, 0, W, H);
        nvg::FillColor(vec4(1.0f, 0.08f, 0.55f, alpha));
        nvg::Fill();

        // Animated horizontal scanlines for extra punch
        nvg::BeginPath();
        float lineAlpha = 0.12f * S_EffectIntensity;
        float lineSpacing = 6.0f;
        float scroll = (t * 40.0f) % (lineSpacing * 2.0f);
        for (float y = -lineSpacing + scroll; y < H; y += lineSpacing * 2.0f) {
            nvg::Rect(0, y, W, lineSpacing);
        }
        nvg::FillColor(vec4(0.0f, 0.0f, 0.0f, lineAlpha));
        nvg::Fill();
    }

    else if (ChaosFX::g_ActiveEffect == ChaosFX::EffectType::Kaleidoscope) {
        // Radiating colored wedges from center
        int segments = 16;
        float cx = W * 0.5f;
        float cy = H * 0.5f;
        float radius = Math::Sqrt(W * W + H * H);
        float angleStep = Math::PI * 2.0f / float(segments);
        float rot = t * 0.4f * S_EffectIntensity;

        for (int i = 0; i < segments; i++) {
            float a0 = rot + i * angleStep;
            float a1 = a0 + angleStep;
            float hue = (float(i) / float(segments) + t * 0.1f) % 1.0f;
            // HSV to RGB approximation for vivid bands
            float r = Math::Abs(hue * 6.0f - 3.0f) - 1.0f;
            float g2 = 2.0f - Math::Abs(hue * 6.0f - 2.0f);
            float b = 2.0f - Math::Abs(hue * 6.0f - 4.0f);
            r = Math::Clamp(r, 0.0f, 1.0f);
            g2 = Math::Clamp(g2, 0.0f, 1.0f);
            b = Math::Clamp(b, 0.0f, 1.0f);
            float alpha = (i % 2 == 0 ? 0.45f : 0.25f) * S_EffectIntensity;

            nvg::BeginPath();
            nvg::MoveTo(vec2(cx, cy));
            nvg::LineTo(vec2(cx + Math::Cos(a0) * radius, cy + Math::Sin(a0) * radius));
            nvg::LineTo(vec2(cx + Math::Cos(a1) * radius, cy + Math::Sin(a1) * radius));
            nvg::ClosePath();
            nvg::FillColor(vec4(r, g2, b, alpha));
            nvg::Fill();
        }
    }

    else if (ChaosFX::g_ActiveEffect == ChaosFX::EffectType::Mirror) {
        // Left-half blue tint + centre mirror line
        float alpha = (Math::Sin(t * 1.8f) * 0.1f + 0.35f) * S_EffectIntensity;
        nvg::BeginPath();
        nvg::Rect(0, 0, W * 0.5f, H);
        nvg::FillColor(vec4(0.3f, 0.6f, 1.0f, alpha));
        nvg::Fill();

        // Right-half warm tint (mirrored feel)
        nvg::BeginPath();
        nvg::Rect(W * 0.5f, 0, W * 0.5f, H);
        nvg::FillColor(vec4(1.0f, 0.6f, 0.3f, alpha * 0.6f));
        nvg::Fill();

        // Centre divider line, pulsing
        float lineAlpha = (Math::Sin(t * 4.0f) * 0.3f + 0.7f) * S_EffectIntensity;
        nvg::BeginPath();
        nvg::MoveTo(vec2(W * 0.5f, 0));
        nvg::LineTo(vec2(W * 0.5f, H));
        nvg::StrokeColor(vec4(1.0f, 1.0f, 1.0f, lineAlpha));
        nvg::StrokeWidth(3.0f);
        nvg::Stroke();
    }
}

void Render() {
    RenderOverlay();

    if (!S_ShowOverlay || !ChaosFX::g_Initialized) return;

    UI::SetNextWindowPos(20, 20, UI::Cond::FirstUseEver);
    UI::SetNextWindowSize(380, 270, UI::Cond::FirstUseEver);

    if (UI::Begin("ChaosFX", S_ShowOverlay)) {
        UI::Text("Status:  " + ChaosFX::g_Status);
        UI::Text("Active:  " + ChaosFX::EffectLabel(ChaosFX::g_ActiveEffect));
        UI::Text("From:    " + ChaosFX::g_ActiveSource);
        UI::Text("Queued:  " + tostring(ChaosFX::g_Queue.Length));

        if (ChaosFX::g_ActiveEffect != ChaosFX::EffectType::None) {
            int left = Math::Max(0, int((ChaosFX::g_EffectEndsAt - Time::Now) / 1000));
            UI::Text("Ends in: " + left + "s");
        }

        UI::Separator();
        UI::Text("Manual test triggers:");

        if (UI::Button("Pink"))         ChaosFX::EnqueueReward("pink_mode",      S_DefaultDurationMs, "manual");
        UI::SameLine();
        if (UI::Button("Kaleidoscope")) ChaosFX::EnqueueReward("kaleidoscope",    S_DefaultDurationMs, "manual");
        UI::SameLine();
        if (UI::Button("Mirror"))       ChaosFX::EnqueueReward("mirrored_screen", S_DefaultDurationMs, "manual");

        UI::Separator();
        if (UI::Button("Clear All")) {
            ChaosFX::g_Queue.Resize(0);
            ChaosFX::g_ActiveEffect = ChaosFX::EffectType::None;
            ChaosFX::g_EffectEndsAt = 0;
            ChaosFX::g_Status = "idle";
            if (ChaosFX::g_DllLoaded && ChaosFX::g_fnClear !is null) { try { ChaosFX::g_fnClear.Call(); } catch {} }
        }
    }
    UI::End();
}
