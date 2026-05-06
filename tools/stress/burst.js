// burst.js — fire N back-to-back ArtDMX packets, sleep, repeat.
// Stresses the UDP RX queue + Lumox's loop() drain. If pollArtNet drops to
// one-per-tick, you'll see the node fall behind here first.
//
//   node burst.js [target] [--count=200] [--gap=500]   (gap in ms between bursts)

const { ART_PORT, buildArtDmx, parseArgs, getTarget, makeSocket, statsPrinter, help }
  = require('./_lib.js');

const args = parseArgs(process.argv);
if (args.flags.h || args.flags.help) {
  help('burst.js', [
    '--count=N   packets per burst (default 200)',
    '--gap=MS    pause between bursts (default 500)',
    '--uni=N     universe (default 0)',
  ]);
  process.exit(0);
}

const target = getTarget(args);
const count  = parseInt(args.flags.count || '200', 10);
const gap    = parseInt(args.flags.gap   || '500', 10);
const uni    = parseInt(args.flags.uni   || '0',   10);

const sock  = makeSocket();
const stats = statsPrinter(`burst ${count}/${gap}ms`);
let seq = 1;
let burstNo = 0;

console.log(`→ ${target}:${ART_PORT}  burst=${count}  gap=${gap}ms  uni=${uni}`);

function fireBurst() {
  burstNo++;
  for (let i = 0; i < count; i++) {
    const data = Buffer.alloc(512, (burstNo + i) & 0xff);
    const pkt  = buildArtDmx({ seq, universe: uni, data });
    sock.send(pkt, ART_PORT, target);
    seq = (seq % 255) + 1;
    stats.tick(pkt.length);
  }
}

fireBurst();
setInterval(fireBurst, gap);

process.on('SIGINT', () => { stats.end(); sock.close(); process.exit(0); });
