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

// The CLI owns song lookup and decoding; it must never open the sound card.
// Scope this adapter to MusicController children with a PCM FIFO configured.
const pcmFifo = process.env.XIAOCAI_MUSIC_PCM_FIFO;
if (pcmFifo) {
  if (!affected) {
    process.stderr.write('Unsupported ncm-cli version for robot PCM decoding\n');
    process.exit(78);
  }
  const childProcess = require('node:child_process');
  const originalSpawn = childProcess.spawn;
  childProcess.spawn = function (command, args = [], options) {
    if (path.basename(String(command)) === 'mpv') {
      args = [...args,
        '--ao=pcm', '--ao-pcm-file=' + pcmFifo, '--ao-pcm-waveheader=no',
        '--audio-format=s16',
        '--audio-samplerate=' + (process.env.XIAOCAI_MUSIC_PCM_RATE || '48000'),
        '--audio-channels=' + (process.env.XIAOCAI_MUSIC_PCM_CHANNELS === '1' ? 'mono' : 'stereo'), '--volume=100', '--loop-file=no', '--loop-playlist=no', '--gapless-audio=no', '--no-video'];
    }
    return originalSpawn.call(this, command, args, options);
  };
  require('node:module').syncBuiltinESMExports();
}
