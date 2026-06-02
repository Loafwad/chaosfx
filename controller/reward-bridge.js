const http = require('http');
const { URL } = require('url');
const fs = require('fs');
const path = require('path');
const { REWARD_MAP } = require('./rewards');

const PORT = Number(process.env.CHAOSFX_BRIDGE_PORT || 18244);
const HOST = process.env.CHAOSFX_BRIDGE_HOST || '127.0.0.1';

const allowedRewards = new Set(Object.values(REWARD_MAP));
const queue = [];
let enabled = true;
let twitchConnected = false;
let twitchLastSeen = 0;

const UI_HTML_PATH = path.join(__dirname, 'ui.html');
const UI_JS_PATH = path.join(__dirname, 'ui.js');

// Inverted REWARD_MAP for the browser: { rewardKey: 'Display Name' }
const REWARD_KEY_NAMES = Object.fromEntries(
    Object.entries(REWARD_MAP).map(([title, key]) => [key, title])
);


function sendJson(res, status, body) {
    const data = JSON.stringify(body);
    res.writeHead(status, {
        'Content-Type': 'application/json; charset=utf-8',
        'Content-Length': Buffer.byteLength(data),
        'Cache-Control': 'no-store'
    });
    res.end(data);
}

function sendEmpty(res, status) {
    res.writeHead(status, { 'Cache-Control': 'no-store' });
    res.end();
}

function parseBody(req) {
    return new Promise((resolve, reject) => {
        let raw = '';
        req.setEncoding('utf8');
        req.on('data', chunk => {
            raw += chunk;
            if (raw.length > 100_000) {
                reject(new Error('request body too large'));
            }
        });
        req.on('end', () => {
            if (!raw) return resolve({});
            try {
                resolve(JSON.parse(raw));
            } catch {
                reject(new Error('invalid json'));
            }
        });
        req.on('error', reject);
    });
}

function normalizeReward(payload) {
    const rewardKey = String(payload.rewardKey || '').trim().toLowerCase();
    if (!allowedRewards.has(rewardKey)) {
        throw new Error('unsupported rewardKey');
    }

    const durationMs = Number.isFinite(payload.durationMs)
        ? Math.max(1000, Math.min(300000, Math.floor(payload.durationMs)))
        : 15000;

    const source = String(payload.source || 'twitch').slice(0, 120);

    return { rewardKey, durationMs, source };
}

async function handleRequest(req, res) {
    const url = new URL(req.url, `http://${req.headers.host || 'localhost'}`);

    if (req.method === 'GET' && url.pathname === '/') {
        const html = fs.readFileSync(UI_HTML_PATH, 'utf8');
        res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
        return res.end(html);
    }

    if (req.method === 'GET' && url.pathname === '/ui.js') {
        const js = fs.readFileSync(UI_JS_PATH, 'utf8');
        res.writeHead(200, { 'Content-Type': 'application/javascript; charset=utf-8', 'Cache-Control': 'no-store' });
        return res.end(js);
    }

    if (req.method === 'GET' && url.pathname === '/rewards/map') {
        return sendJson(res, 200, REWARD_KEY_NAMES);
    }

    if (req.method === 'GET' && url.pathname === '/health') {
        // Twitch is considered connected if it sent a heartbeat in the last 60s
        const tc = twitchConnected && (Date.now() - twitchLastSeen < 60_000);
        return sendJson(res, 200, { ok: true, queued: queue.length, enabled, twitchConnected: tc });
    }

    if (req.method === 'POST' && url.pathname === '/enable') {
        enabled = true;
        return sendJson(res, 200, { enabled });
    }

    if (req.method === 'POST' && url.pathname === '/disable') {
        enabled = false;
        return sendJson(res, 200, { enabled });
    }

    if (req.method === 'POST' && url.pathname === '/twitch/heartbeat') {
        twitchConnected = true;
        twitchLastSeen = Date.now();
        return sendEmpty(res, 204);
    }

    if (req.method === 'POST' && url.pathname === '/twitch/disconnect') {
        twitchConnected = false;
        return sendEmpty(res, 204);
    }

    if (req.method === 'GET' && url.pathname === '/rewards/next') {
        if (!enabled || queue.length === 0) {
            return sendEmpty(res, 204);
        }

        const next = queue.shift();
        return sendJson(res, 200, next);
    }

    if (req.method === 'POST' && url.pathname === '/rewards/enqueue') {
        let payload;
        try {
            payload = await parseBody(req);
        } catch (err) {
            return sendJson(res, 400, { error: err.message });
        }

        let event;
        try {
            event = normalizeReward(payload);
        } catch (err) {
            return sendJson(res, 400, { error: err.message });
        }

        queue.push(event);
        return sendJson(res, 202, { accepted: true, queued: queue.length, event });
    }

    if (req.method === 'POST' && url.pathname === '/rewards/clear') {
        queue.length = 0;
        return sendJson(res, 200, { cleared: true });
    }

    return sendJson(res, 404, { error: 'not found' });
}

const server = http.createServer((req, res) => {
    handleRequest(req, res).catch(err => {
        sendJson(res, 500, { error: err.message || 'internal error' });
    });
});

server.listen(PORT, HOST, () => {
    console.log(`[ChaosFX bridge] listening on http://${HOST}:${PORT}`);
});
