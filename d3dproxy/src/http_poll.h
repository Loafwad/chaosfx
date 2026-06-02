#pragma once

// Starts a background thread that polls the reward bridge on
// http://127.0.0.1:18244/rewards/next and drives chaosfx::effects.
void Proxy_StartPolling();
