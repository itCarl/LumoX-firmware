// sync.js — drive ArtSync mode. Sends ArtDMX then immediately ArtSync, so
// /api/status should report artnet.inSync=true and artnet.syncs incrementing.
// Also flips back to non-sync after ~4 s if you stop with --once.
//
//   node sync.js [target] [--hz=30] [--once]

const { ART_PORT, buildArtDmx, buildArtSync, parseArgs, getTarget, makeSocket,
        statsPrinter, help } = require('./_lib.js');

const args = parseArgs(process.argv);
if (args.flags.h || args.flags.help) {
  help('sync.js', [
    '--hz=N      DMX+Sync pair rate (default 30)',
    '--once      send a single pair then exit (tests sync timeout revert)',
  ]);
  process.exit(0);
}

const target = getTarget(args);
const hz     = parseInt(args.flags.hz || '30', 10);
const once   = !!args.flags.once;

const sock  = makeSocket();
const stats = statsPrinter(`sync ${hz}Hz`);
const period = 1000 / hz;
const data = Buffer.alloc(512);
const syncPkt = buildArtSync();
let seq = 1, phase = 0;

console.log(`→ ${target}:${ART_PORT}  ArtDMX+ArtSync pairs @ ${hz}Hz${once ? ' (once)' : ''}`);

function emit() {
  for (let i = 0; i < 512; i++) data[i] = ((Math.sin((phase + i) * 0.1) + 1) * 127) | 0;
  phase += 5;
  const dmx = buildArtDmx({ seq, universe: 0, data });
  sock.send(dmx, ART_PORT, target);
  sock.send(syncPkt, ART_PORT, target);
  seq = (seq % 255) + 1;
  stats.tick(dmx.length + syncPkt.length);
}

if (once) {
  emit();
  console.log('\nsent one pair — node should drop sync mode in ~4 s');
  setTimeout(() => { stats.end(); sock.close(); process.exit(0); }, 200);
} else {
  setInterval(emit, period);
}

process.on('SIGINT', () => { stats.end(); sock.close(); process.exit(0); });
