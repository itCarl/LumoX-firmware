// pollstorm.js — spam OpPoll at high rate.
// Forces the node to build + emit ArtPollReply packets. Tests the reply
// builder, the per-netif broadcast helper, and (with the MAC cache fix) the
// hot path of sendArtPollReply().
//
//   node pollstorm.js [target] [--hz=50]
// Reply traffic is broadcast — bind a listener on the same machine to count:
//   node pollstorm.js 192.168.178.105 --hz=50 --listen

const { ART_PORT, buildArtPoll, parseArgs, getTarget, makeSocket, statsPrinter, help }
  = require('./_lib.js');
const dgram = require('dgram');

const args = parseArgs(process.argv);
if (args.flags.h || args.flags.help) {
  help('pollstorm.js', [
    '--hz=N      polls per second (default 50)',
    '--listen    also bind 6454 and count incoming ArtPollReply',
  ]);
  process.exit(0);
}

const target = getTarget(args);
const hz     = parseInt(args.flags.hz || '50', 10);
const listen = !!args.flags.listen;

const sock  = makeSocket();
const stats = statsPrinter(`pollstorm ${hz}Hz`);

console.log(`→ ${target}:${ART_PORT}  ArtPoll @ ${hz}Hz`);

if (listen) {
  const rx = dgram.createSocket('udp4');
  rx.bind(ART_PORT, () => rx.setBroadcast(true));
  let replies = 0;
  rx.on('message', (msg, rinfo) => {
    if (msg.length >= 10 && msg.slice(0, 8).toString('ascii') === 'Art-Net\0'
        && msg.readUInt16LE(8) === 0x2100) {
      replies++;
      if (replies % 25 === 0) {
        process.stdout.write(`\n  ← ${replies} ArtPollReply from ${rinfo.address}\n`);
      }
    }
  });
}

const pkt = buildArtPoll();
setInterval(() => { sock.send(pkt, ART_PORT, target); stats.tick(pkt.length); }, 1000 / hz);

process.on('SIGINT', () => { stats.end(); sock.close(); process.exit(0); });
