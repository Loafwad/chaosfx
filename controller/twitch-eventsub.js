'use strict';
require('dotenv').config();

// ChaosFX — Twitch EventSub WebSocket listener
//
// Subscribes to channel point redemptions and forwards them to the local
// reward bridge, which the Openplanet plugin polls every second.
//
// ── Quick start ──────────────────────────────────────────────────────────────
//   1. npm install          (in controller/)
//   2. Set the env vars below (or export them in your shell)
//   3. node twitch-eventsub.js    (keep running alongside reward-bridge.js)
// ─────────────────────────────────────────────────────────────────────────────
//
// Required env vars:
//   TWITCH_CLIENT_ID       Your Twitch app Client ID  (dev.twitch.tv → Your Console)
//   TWITCH_ACCESS_TOKEN    User access token with scope: channel:read:redemptions
//   TWITCH_BROADCASTER_ID  Your numeric broadcaster user ID
//                          (look up at: https://www.streamweasels.com/tools/convert-twitch-username-to-user-id/)
//
// Optional env vars:
//   CHAOSFX_BRIDGE_URL          Default: http://127.0.0.1:18244
//   REWARD_DURATION_MS          How long each effect lasts in ms.  Default: 15000
//
//   The following map your exact Twitch reward titles to effect keys.
//   Edit the defaults if your reward names differ.
//   REWARD_TITLE_PINK_MODE       Default: "Pink Mode"
//   REWARD_TITLE_KALEIDOSCOPE    Default: "Kaleidoscope"
//   REWARD_TITLE_MIRRORED_SCREEN Default: "Mirrored Screen"

const WebSocket = require('ws');
const https = require('https');
const http = require('http');
const { REWARD_MAP } = require('./rewards');


const CLIENT_ID = process.env.TWITCH_CLIENT_ID || '';
const ACCESS_TOKEN = process.env.TWITCH_ACCESS_TOKEN || '';
const BROADCASTER_ID = process.env.TWITCH_BROADCASTER_ID || '';
const BRIDGE_URL = process.env.CHAOSFX_BRIDGE_URL || 'http://127.0.0.1:18244';
const DURATION_MS = Number(process.env.REWARD_DURATION_MS) || 15000;

if (!CLIENT_ID || !ACCESS_TOKEN || !BROADCASTER_ID) {
    console.error(
        '[ChaosFX] Missing required env vars.\n' +
        '  TWITCH_CLIENT_ID, TWITCH_ACCESS_TOKEN, TWITCH_BROADCASTER_ID must all be set.'
    );
    process.exit(1);
}

// ── Helpers ──────────────────────────────────────────────────────────────────

function log(msg) {
    console.log(`[${new Date().toISOString()}] ${msg}`);
}

// POST a one-shot fire-and-forget request to the local bridge.
function postToBridge(path, body) {
    const data = body ? JSON.stringify(body) : null;
    const parsed = new URL(`${BRIDGE_URL}${path}`);
    const transport = parsed.protocol === 'https:' ? https : http;
    const req = transport.request({
        hostname: parsed.hostname,
        port: parsed.port || (parsed.protocol === 'https:' ? 443 : 80),
        path: parsed.pathname,
        method: 'POST',
        headers: data ? {
            'Content-Type': 'application/json',
            'Content-Length': Buffer.byteLength(data),
        } : {},
    }, res => { res.resume(); });
    req.on('error', () => { });
    if (data) req.write(data);
    req.end();
}

// POST a reward event to the local bridge server.
function postRewardToBridge(rewardKey, source) {
    const body = { rewardKey, durationMs: DURATION_MS, source };
    postToBridge('/rewards/enqueue', body);
    log(`[bridge] enqueue ${rewardKey} from ${source}`);
}

// Subscribe to channel point redemptions via the Helix EventSub REST API.
// Must be called after we receive session_welcome with a valid session_id.
function subscribeToRedemptions(sessionId) {
    const body = JSON.stringify({
        type: 'channel.channel_points_custom_reward_redemption.add',
        version: '1',
        condition: { broadcaster_user_id: BROADCASTER_ID },
        transport: { method: 'websocket', session_id: sessionId },
    });

    const req = https.request({
        hostname: 'api.twitch.tv',
        path: '/helix/eventsub/subscriptions',
        method: 'POST',
        headers: {
            'Client-Id': CLIENT_ID,
            'Authorization': `Bearer ${ACCESS_TOKEN}`,
            'Content-Type': 'application/json',
            'Content-Length': Buffer.byteLength(body),
        },
    }, res => {
        let data = '';
        res.on('data', chunk => data += chunk);
        res.on('end', () => {
            if (res.statusCode === 202) {
                log('[twitch] EventSub subscription active — listening for redemptions');
            } else {
                log(`[twitch] Subscription failed HTTP ${res.statusCode}: ${data}`);
                if (res.statusCode === 401) {
                    log('[twitch] Token expired or missing scope. Re-generate with channel:read:redemptions.');
                }
            }
        });
    });

    req.on('error', err => log(`[twitch] Helix request error: ${err.message}`));
    req.write(body);
    req.end();
}

// Handle a parsed EventSub message object.
function handleMessage(msg) {
    const type = msg?.metadata?.message_type;

    if (type === 'session_welcome') {
        const sessionId = msg.payload?.session?.id;
        log(`[twitch] session_welcome  session_id=${sessionId}`);
        subscribeToRedemptions(sessionId);
        postToBridge('/twitch/heartbeat', null);
    }

    else if (type === 'session_keepalive') {
        postToBridge('/twitch/heartbeat', null);
    }

    else if (type === 'session_reconnect') {
        // Twitch is asking us to reconnect to a new URL.
        const url = msg.payload?.session?.reconnect_url;
        log(`[twitch] session_reconnect → ${url}`);
        connect(url);
    }

    else if (type === 'notification') {
        const event = msg.payload?.event;
        const title = event?.reward?.title ?? '';
        const user = event?.user_name ?? 'unknown';
        const rewardKey = REWARD_MAP[title];

        if (!rewardKey) {
            log(`[twitch] unrecognised reward title "${title}" — not mapped, ignoring`);
            return;
        }

        log(`[twitch] redemption: "${title}" by ${user} → ${rewardKey}`);
        postRewardToBridge(rewardKey, `twitch:${user}`);
    }

    else if (type === 'revocation') {
        log(`[twitch] subscription revoked (status: ${msg.payload?.subscription?.status})`);
    }
}

// ── Connection ───────────────────────────────────────────────────────────────

let reconnectDelay = 2000;

function connect(url = 'wss://eventsub.wss.twitch.tv/ws') {
    log(`[twitch] connecting to ${url}`);
    const ws = new WebSocket(url);

    ws.on('open', () => {
        log('[twitch] WebSocket open');
        reconnectDelay = 2000; // reset backoff on successful connect
    });

    ws.on('message', raw => {
        let msg;
        try { msg = JSON.parse(raw); } catch { return; }
        handleMessage(msg);
    });

    ws.on('close', (code, reason) => {
        log(`[twitch] WebSocket closed  code=${code}  — reconnecting in ${reconnectDelay}ms`);
        postToBridge('/twitch/disconnect', null);
        setTimeout(() => connect(), reconnectDelay);
        reconnectDelay = Math.min(reconnectDelay * 2, 30_000); // exponential backoff, cap 30s
    });

    ws.on('error', err => {
        log(`[twitch] WebSocket error: ${err.message}`);
        // 'close' fires after 'error', so the reconnect is handled there.
    });
}

// ── Start ────────────────────────────────────────────────────────────────────

log('[ChaosFX] Twitch EventSub listener starting');
log(`[ChaosFX] Reward map: ${JSON.stringify(REWARD_MAP, null, 2)}`);
connect();
