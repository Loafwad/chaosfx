let _enabled = true;
let _names = {}; // populated from /rewards/map on init

async function init() {
    try {
        const map = await fetch("/rewards/map").then((r) => r.json());
        // map is { rewardKey: "Display Name", ... }
        _names = map;
    } catch (e) { }
    poll();
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
        const btn = document.getElementById("toggle-btn");
        btn.textContent = _enabled ? "Enabled ✓" : "Disabled";
        btn.className = "btn min-w-28 " + (_enabled ? "btn-success" : "btn-error");
    } catch (e) { }
    setTimeout(poll, 2000);
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
