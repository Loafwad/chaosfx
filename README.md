# ChaosFX (Openplanet Path)

This project is now pivoted toward an Openplanet plugin workflow to stay aligned with Trackmania online/TOS expectations.

## Current Focus

- Build the feature as an AngelScript Openplanet plugin.
- Avoid direct DLL injection workflow.
- Keep development in Openplanet developer mode for safe testing.

## Openplanet Plugin Scaffold

- Plugin folder: `openplanet/ChaosFX`
- Manifest: `openplanet/ChaosFX/info.toml`
- Script entrypoint: `openplanet/ChaosFX/src/Main.as`

The script provides:

- Wrapper-style lifecycle functions (`Initialize` and `Shutdown`)
- `Main()` and `OnDestroyed()` plugin lifecycle wiring
- Timed effect engine with queue and active effect state
- Full-screen visual effects for `pink_mode`, `kaleidoscope`, and `mirrored_screen`
- Local reward bridge polling (`http://127.0.0.1:18244/rewards/next`)

## Important Note About DLL Imports

The following AngelScript pattern is **not** the recommended Openplanet path:

```angelscript
import void InitializeDX11Hook() from "MyCustomPipeline.dll";
```

Openplanet plugins should be implemented through the Openplanet API and supported extension paths, not by loading arbitrary DLL hooks from script.

## Run Steps

1. Install Openplanet and enable developer mode.
2. Start the local reward bridge service:

```bash
node controller/reward-bridge.js
```

2. Copy `openplanet/ChaosFX` into your Openplanet plugins folder.
3. Launch Trackmania with Openplanet.
4. Enable the `ChaosFX` plugin.
5. Open the ChaosFX window and test manual triggers (`Pink`, `Kaleidoscope`, `Mirror`).

You can enqueue test events while the game is running:

```bash
curl -X POST http://127.0.0.1:18244/rewards/enqueue \
	-H "content-type: application/json" \
	-d '{"rewardKey":"pink_mode","durationMs":15000,"source":"twitch:test_user"}'
```

## Twitch Rewards Bridge Contract

Openplanet plugin calls:

- `GET http://127.0.0.1:18244/rewards/next`

Expected behavior:

- `204` when no events are queued
- `200` with JSON object when one reward event is available

Example response body:

```json
{
	"rewardKey": "pink_mode",
	"durationMs": 15000,
	"source": "twitch:viewer_name"
}
```

Supported `rewardKey` values:

- `pink_mode`
- `kaleidoscope`
- `mirrored_screen`

Recommended architecture:

- Run Twitch EventSub / channel point handling in a separate local service.
- Translate reward redemptions to the JSON contract above.
- Keep Openplanet plugin focused on in-game effect orchestration/rendering.

## Legacy Rust Injector Code

The Rust crates in this repo are retained as legacy experimentation only and are no longer the primary path.
