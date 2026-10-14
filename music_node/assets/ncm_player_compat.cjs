'use strict';

// ncm-cli 0.1.7 gives daemon requests only 3 seconds, including IP discovery,
// signed song requests and MPV startup. On the board this expires before the
// player is ready and terminates the controller. Keep the installed CLI intact;
// preload this adapter only in the robot runtime. Recheck on CLI upgrades.
const fs = require('node:fs');
const path = require('node:path');

let affected = false;
try {
  const entry = fs.realpathSync(process.argv[1]);
  const directory = path.dirname(entry);
  const manifest = JSON.parse(
    fs.readFileSync(path.join(directory, '..', 'package.json'), 'utf8'));
  affected = manifest.name === '@music163/ncm-cli' && manifest.version === '0.1.7';
} catch {
  // Other Node programs and CLI versions keep their original timers.
}

if (affected) {
  const originalSetTimeout = global.setTimeout;
  global.setTimeout = function (callback, delay, ...args) {
    return originalSetTimeout(callback, delay === 3000 ? 20000 : delay, ...args);
  };
}
