'use strict';

// Canonical map of Twitch reward titles → ChaosFX effect keys.
// Both reward-bridge.js (allowedRewards validation) and
// twitch-eventsub.js (title lookup) import from here.
const REWARD_MAP = {
    'Pink Mode': 'pink_mode',
    'Kaleidoscope': 'kaleidoscope',
    'Mirrored Screen': 'mirrored_screen',
    'Flipped Screen': 'flipped_screen',
};

module.exports = { REWARD_MAP };
