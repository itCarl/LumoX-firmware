// stuckseq.js — send valid ArtDMX with a constant seq byte.
// Verifies the seq=duplicate fix. With the old `<= 0` rule the node rejected
// 80%+ of these as stale; with `< 0` virtually all should be accepted.
//
//   node stuckseq.js [target] [--seq=1] [--hz=44]

const { ART_PORT, buildArtDmx, parseArgs, getTarget, makeSocket, statsPrinter, help }
  = require('./_lib.js');

const args = parseArgs(process.argv);
if (args.flags.h || args.flags.help) {
  help('stuckseq.js', [
    '--seq=N     fixed seq byte 1..255 (default 1; 0 disables seq check)',
    '--hz=N      send rate (default 44)',
  ]);
  process.exit(0);
}

const target = getTarget(args);
const seq    = parseInt(args.flags.seq || '1',  10) & 0xff;
const hz     = parseInt(args.flags.hz  || '44', 10);

const sock  = makeSocket();
const stats = statsPrinter(`stuckseq seq=${seq}`);
const period = 1000 / hz;
const data = Buffer.alloc(512);
let v = 0;

console.log(`→ ${target}:${ART_PORT}  fixed seq=${seq}  ${hz}Hz`);
console.log('  watch /api/status: artnet.stale should NOT keep climbing.');

setInterval(() => {
  v = (v + 1) & 0xff;
  data.fill(v);
  const pkt = buildArtDmx({ seq, universe: 0, data });
  sock.send(pkt, ART_PORT, target);
  stats.tick(pkt.length);
}, period);

process.on('SIGINT', () => { stats.end(); sock.close(); process.exit(0); });
