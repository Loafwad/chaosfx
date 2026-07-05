let _enabled = true;
let _names = {}; // populated from /rewards/map on init

// ── draw call debug state ────────────────────────────────────────────────────
let _skipDrawN = -1;         // current value as known by the bridge
let _lastSkipTs = null;       // timestamp of the last captured skip info

async function init() {
    try {
        const map = await fetch("/rewards/map").then((r) => r.json());
        // map is { rewardKey: "Display Name", ... }
        _names = map;
    } catch (e) { }
    poll();
    pollDebug();
}

async function poll() {
    try {
        const d = await fetch("/health").then((r) => r.json());
        document.getElementById("tw-badge").className =
            "badge gap-2 " + (d.twitchConnected ? "badge-success" : "badge-error");
        document.getElementById("tw-label").textContent = d.twitchConnected
            ? "Twitch connected"
            : "Twitch disconnected";
        document.getElementById("queued").textContent = d.queued;
        _enabled = d.enabled;
        const gameBadge = document.getElementById("game-badge");
        gameBadge.className = "badge gap-2 " + (d.gameRunning ? "badge-success" : "badge-error");
        document.getElementById("game-label").textContent = d.gameRunning ? "Game running" : "Game not running";
        const btn = document.getElementById("toggle-btn");
        btn.textContent = _enabled ? "Enabled ✓" : "Disabled";
        btn.className = "btn min-w-28 " + (_enabled ? "btn-success" : "btn-error");
    } catch (e) { }
    setTimeout(poll, 2000);
}

async function pollDebug() {
    try {
        const d = await fetch("/debug/state").then((r) => r.json());

        // Update skip-N display (don't overwrite while user is focused)
        _skipDrawN = d.skipDrawN;
        const skipEl = document.getElementById("skip-n");
        if (document.activeElement !== skipEl)
            skipEl.value = _skipDrawN >= 0 ? String(_skipDrawN) : "—";

        // Frame stats from C++
        if (d.frameStats) {
            document.getElementById("draws-per-frame").textContent = d.frameStats.drawsPerFrame;
            document.getElementById("active-skip-n").textContent =
                d.frameStats.activeSkipN >= 0 ? String(d.frameStats.activeSkipN) : "off";
            document.getElementById("hooks-installed").textContent =
                d.frameStats.hooksInstalled ? "✓" : "✗";
        }

        // Show last-skip info if it's new
        if (d.lastSkip && d.lastSkip.ts !== _lastSkipTs) {
            _lastSkipTs = d.lastSkip.ts;
            const info = d.lastSkip;
            const text =
                `idx: ${info.indexCount}  start: ${info.startIndex}\n` +
                `vs:  ${info.vs}\n` +
                `ps:  ${info.ps}`;
            const infoEl = document.getElementById("skip-info");
            document.getElementById("skip-info-text").textContent = text;
            infoEl.classList.remove("hidden");

            // Append to log
            const log = document.getElementById("skip-log");
            const entry = document.createElement("div");
            const t = new Date(info.ts);
            entry.className = "flex gap-2";
            entry.innerHTML =
                `<span class="text-base-content/30">${t.toLocaleTimeString()}</span>` +
                `<span class="text-base-content/50">#${_skipDrawN}</span>` +
                `<span class="truncate">${info.vs} / ${info.ps}</span>`;
            log.prepend(entry);
            // Keep at most 50 entries
            while (log.children.length > 50) log.removeChild(log.lastChild);
        }
    } catch (e) { }
    setTimeout(pollDebug, 1000);
}

function skipCopyInfo() {
    const text = document.getElementById("skip-info-text").textContent;
    if (text) navigator.clipboard.writeText(text).catch(() => { });
}

async function skipAdjust(delta) {
    const next = Math.max(-1, _skipDrawN + delta);
    await fetch("/debug/skip-draw", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ n: next }),
    }).catch(() => { });
    // Optimistic update (pollDebug will confirm)
    _skipDrawN = next;
    document.getElementById("skip-n").value =
        _skipDrawN >= 0 ? String(_skipDrawN) : "—";
}

async function skipSetDirect(raw) {
    const n = parseInt(raw, 10);
    if (isNaN(n) || n < 0) { await skipClear(); return; }
    await fetch("/debug/skip-draw", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ n }),
    }).catch(() => { });
    _skipDrawN = n;
    document.getElementById("skip-n").value = String(n);
}

async function skipClear() {
    await fetch("/debug/skip-draw", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ n: -1 }),
    }).catch(() => { });
    _skipDrawN = -1;
    document.getElementById("skip-n").value = "—";
    document.getElementById("skip-info").classList.add("hidden");
}

// ── Shader histogram ─────────────────────────────────────────────────────────
let _skipPairs = new Set(); // "vs|ps" keys

function _pairKey(vs, ps) { return vs + '|' + ps; }

async function _sendSkipPairs() {
    const pairs = [..._skipPairs].map(k => { const [vs, ps] = k.split('|'); return { vs, ps }; });
    await fetch('/debug/skip-shader', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ pairs }),
    }).catch(() => { });
    document.getElementById('active-shader').textContent =
        _skipPairs.size === 0 ? 'none' : `${_skipPairs.size} pair(s) suppressed`;
}

function _refreshHistHighlights() {
    const table = document.getElementById('hist-table');
    for (const row of table.children) {
        if (!row.dataset.vs) continue;
        const active = _skipPairs.has(_pairKey(row.dataset.vs, row.dataset.ps));
        row.className = 'flex gap-2 cursor-pointer hover:bg-base-100 rounded px-1' +
            (active ? ' bg-primary/20 text-primary font-bold' : '');
    }
}

async function loadHist() {
    const d = await fetch('/debug/shader-hist').then(r => r.json()).catch(() => null);
    if (!d || !Array.isArray(d.pairs)) return;
    const table = document.getElementById('hist-table');
    if (d.pairs.length === 0) {
        table.innerHTML = '<div class="text-base-content/30 italic">no data yet</div>';
        return;
    }
    table.innerHTML = '';
    for (const p of d.pairs) {
        const row = document.createElement('div');
        row.dataset.vs = p.vs;
        row.dataset.ps = p.ps;
        const active = _skipPairs.has(_pairKey(p.vs, p.ps));
        row.className = 'flex gap-2 cursor-pointer hover:bg-base-100 rounded px-1' +
            (active ? ' bg-primary/20 text-primary font-bold' : '');
        row.title = 'Click to toggle suppression of this shader pair';
        row.innerHTML =
            `<span class="w-10 text-right shrink-0 text-base-content/50">${p.n}×</span>` +
            `<span class="truncate">${p.vs} / ${p.ps}</span>`;
        row.addEventListener('click', () => skipShaderPair(p.vs, p.ps));
        table.appendChild(row);
    }
}

async function skipShaderPair(vs, ps) {
    const key = _pairKey(vs, ps);
    if (_skipPairs.has(key)) _skipPairs.delete(key);
    else _skipPairs.add(key);
    await _sendSkipPairs();
    _refreshHistHighlights();
}

async function clearSkipPairs() {
    _skipPairs.clear();
    await _sendSkipPairs();
    _refreshHistHighlights();
}

// ── Index-count skip ────────────────────────────────────────────────────
let _skipIndexCounts = new Set();

function _refreshIdxHistHighlights() {
    const table = document.getElementById('idx-hist-table');
    for (const row of table.children) {
        if (!row.dataset.c) continue;
        const active = _skipIndexCounts.has(Number(row.dataset.c));
        row.className = 'flex gap-2 cursor-pointer hover:bg-base-100 rounded px-1' +
            (active ? ' bg-secondary/20 text-secondary font-bold' : '');
    }
}

async function _sendSkipIndexCounts() {
    const counts = [..._skipIndexCounts];
    await fetch('/debug/skip-index-count', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ counts }),
    }).catch(() => { });
    document.getElementById('active-idx-count').textContent =
        counts.length === 0 ? 'none' : `${counts.length} count(s) suppressed`;
}

async function loadIdxHist() {
    const d = await fetch('/debug/index-count-hist').then(r => r.json()).catch(() => null);
    if (!d || !Array.isArray(d.pairs)) return;
    const table = document.getElementById('idx-hist-table');
    if (d.pairs.length === 0) {
        table.innerHTML = '<div class="text-base-content/30 italic">no data yet</div>';
        return;
    }
    table.innerHTML = '';
    for (const p of d.pairs) {
        const row = document.createElement('div');
        row.dataset.c = p.c;
        const active = _skipIndexCounts.has(p.c);
        row.className = 'flex gap-2 cursor-pointer hover:bg-base-100 rounded px-1' +
            (active ? ' bg-secondary/20 text-secondary font-bold' : '');
        row.title = 'Click to toggle suppression of draws with this index count';
        row.innerHTML =
            `<span class="w-10 text-right shrink-0 text-base-content/50">${p.n}×</span>` +
            `<span class="truncate font-mono">idx=${p.c}</span>`;
        row.addEventListener('click', () => toggleSkipIndexCount(p.c));
        table.appendChild(row);
    }
}

async function toggleSkipIndexCount(c) {
    if (_skipIndexCounts.has(c)) _skipIndexCounts.delete(c);
    else _skipIndexCounts.add(c);
    await _sendSkipIndexCounts();
    _refreshIdxHistHighlights();
}

async function clearSkipIndexCounts() {
    _skipIndexCounts.clear();
    await _sendSkipIndexCounts();
    _refreshIdxHistHighlights();
}

async function toggleEnabled() {
    await fetch(_enabled ? "/disable" : "/enable", { method: "POST" });
    poll();
}

async function clearQueue() {
    await fetch("/rewards/clear", { method: "POST" });
    poll();
}

async function trigger(key) {
    const secs = Math.max(
        1,
        parseInt(document.getElementById("dur").value) || 10
    );
    await fetch("/rewards/enqueue", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
            rewardKey: key,
            durationMs: secs * 1000,
            source: "manual",
        }),
    });
    const label = _names[key] || key;
    document.getElementById("last-trigger").textContent =
        "Triggered " + label + " for " + secs + "s";
    setTimeout(
        () => (document.getElementById("last-trigger").textContent = ""),
        secs * 1000
    );
    poll();
}

init();
