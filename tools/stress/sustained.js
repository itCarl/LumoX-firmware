// sustained.js — send ArtDMX at a fixed Hz with a slow color sweep.
// Validates basic throughput + steady-state DMX rate on the node.
//
//   node sustained.js [target] [--hz=44] [--uni=0] [--ch=512]

const { ART_PORT, buildArtDmx, parseArgs, getTarget, makeSocket, statsPrinter, help }
  = require('./_lib.js');

const args = parseArgs(process.argv);
if (args.flags.h || args.flags.help) {
  help('sustained.js', [
    '--hz=N      target frame rate (default 44)',
    '--uni=N     Art-Net universe (default 0)',
    '--ch=N      number of channels in each frame (1..512, default 512)',
  ]);
  process.exit(0);
}

const target = getTarget(args);
const hz     = parseInt(args.flags.hz  || '44',  10);
const uni    = parseInt(args.flags.uni || '0',   10);
const chCnt  = Math.max(1, Math.min(512, parseInt(args.flags.ch || '512', 10)));

const sock  = makeSocket();
const stats = statsPrinter(`sustained ${hz}Hz uni${uni}`);
const periodMs = 1000 / hz;
const data = Buffer.alloc(chCnt);
let seq = 1;
let phase = 0;

console.log(`→ ${target}:${ART_PORT}  ${hz}Hz  uni=${uni}  ch=${chCnt}`);

setInterval(() => {
  // Sine sweep across all channels — gives the node visible work.
  for (let i = 0; i < chCnt; i++) {
    data[i] = ((Math.sin((phase + i) * 0.05) + 1) * 127) | 0;
  }
  phase += 2;

  const pkt = buildArtDmx({ seq, universe: uni, data });
  sock.send(pkt, ART_PORT, target);
  seq = (seq % 255) + 1;
  stats.tick(pkt.length);
}, periodMs);

process.on('SIGINT', () => { stats.end(); sock.close(); process.exit(0); });
