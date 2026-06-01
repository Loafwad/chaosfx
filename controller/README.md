# ChaosFX Reward Bridge

Minimal local HTTP bridge for feeding reward events into the Openplanet plugin.

## Start

```bash
node controller/reward-bridge.js
```

Defaults:

- Host: `127.0.0.1`
- Port: `18244`

Environment overrides:

- `CHAOSFX_BRIDGE_HOST`
- `CHAOSFX_BRIDGE_PORT`

## Endpoints

- `GET /health`
- `GET /rewards/next`
- `POST /rewards/enqueue`
- `POST /rewards/clear`

## Enqueue Example

```bash
curl -X POST http://127.0.0.1:18244/rewards/enqueue \
  -H "content-type: application/json" \
  -d '{"rewardKey":"pink_mode","durationMs":15000,"source":"twitch:test_user"}'
```

Valid reward keys:

- `pink_mode`
- `kaleidoscope`
- `mirrored_screen`
