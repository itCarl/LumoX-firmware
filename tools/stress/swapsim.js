// swapsim.js — single-sender lockout exerciser. Same source IP, but pauses
// long enough between bursts that the node's stale window expires (>2s by
// default), letting it accept the "next sender". You'll see locked=0 and
// swaps=0 because real swap detection requires two distinct source IPs —
// this script simulates the pause/resume pattern to confirm the lock RELEASES
// cleanly. For a real swap test, run this from one machine + sustained.js
// from another simultaneously.
//
//   node swapsim.js [target] [--burst=2000ms] [--gap=3000ms]

const { ART_PORT, buildArtDmx, parseArgs, getTarget, makeSocket, statsPrinter, help }
  = require('./_lib.js');

const args = parseArgs(process.argv);
if (args.flags.h || args.flags.help) {
  help('swapsim.js', [
    '--burst=MS  send-active window (default 2000)',
    '--gap=MS    silence window between bursts (default 3000, > stale window)',
  ]);
  process.exit(0);
}

const target = getTarget(args);
const burst  = parseInt(args.flags.burst || '2000', 10);
const gap    = parseInt(args.flags.gap   || '3000', 10);

const sock  = makeSocket();
const stats = statsPrinter('swapsim');
let seq = 1;

console.log(`→ ${target}:${ART_PORT}  burst=${burst}ms / gap=${gap}ms`);
console.log('  watch /api/status: artnet.age should peak ≈gap, then reset.');

let timer = null;
function startBurst() {
  console.log(`\n[${new Date().toISOString().slice(11,19)}] burst ON`);
  timer = setInterval(() => {
    const data = Buffer.alloc(512, seq);
    const pkt  = buildArtDmx({ seq, universe: 0, data });
    sock.send(pkt, ART_PORT, target);
    seq = (seq % 255) + 1;
    stats.tick(pkt.length);
  }, 22);
  setTimeout(stopBurst, burst);
}
function stopBurst() {
  clearInterval(timer); timer = null;
  console.log(`\n[${new Date().toISOString().slice(11,19)}] silence`);
  setTimeout(startBurst, gap);
}
startBurst();

process.on('SIGINT', () => { if (timer) clearInterval(timer); stats.end(); sock.close(); process.exit(0); });
