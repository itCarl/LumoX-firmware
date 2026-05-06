// mixed.js — concurrent ArtDMX + ArtPoll + WebSocket + HTTP /api/dmx polling
// against the same node. Stresses mutex contention between Art-Net parser,
// DMX TX task, and AsyncWebServer. Pair with /health mutex log to see
// when spikes correlate with the HTTP poller.
//
//   node mixed.js [target] [--hz=44] [--http-hz=10] [--ws]

const { ART_PORT, buildArtDmx, buildArtPoll, parseArgs, getTarget, makeSocket,
        statsPrinter, help } = require('./_lib.js');
const http = require('http');

const args = parseArgs(process.argv);
if (args.flags.h || args.flags.help) {
  help('mixed.js', [
    '--hz=N        ArtDMX rate (default 44)',
    '--http-hz=N   /api/dmx GET rate (default 10)',
    '--ws          also open a /ws WebSocket client (uses node:net)',
  ]);
  process.exit(0);
}

const target  = getTarget(args);
const hz      = parseInt(args.flags.hz       || '44', 10);
const httpHz  = parseInt(args.flags['http-hz'] || '10', 10);
const useWs   = !!args.flags.ws;

const sock = makeSocket();
const sDmx = statsPrinter('dmx');
const sHttp = { n: 0, err: 0 };
let seq = 1, phase = 0;

console.log(`→ ${target}:${ART_PORT}  ArtDMX@${hz}Hz + /api/dmx@${httpHz}Hz${useWs ? ' + ws' : ''}`);

// ── ArtDMX sender ─────────────────────────────────────────────────────────
const data = Buffer.alloc(512);
setInterval(() => {
  for (let i = 0; i < 512; i++) data[i] = ((Math.sin((phase + i) * 0.07) + 1) * 127) | 0;
  phase += 3;
  const pkt = buildArtDmx({ seq, universe: 0, data });
  sock.send(pkt, ART_PORT, target);
  seq = (seq % 255) + 1;
  sDmx.tick(pkt.length);
}, 1000 / hz);

// Sprinkle ArtPolls to keep ArtPollReply path warm
setInterval(() => sock.send(buildArtPoll(), ART_PORT, target), 500);

// ── HTTP poller ───────────────────────────────────────────────────────────
function pollHttp() {
  const req = http.get({ host: target, port: 80, path: '/api/dmx', timeout: 1000 }, (res) => {
    res.on('data', () => {});
    res.on('end', () => sHttp.n++);
  });
  req.on('error',   () => sHttp.err++);
  req.on('timeout', () => { req.destroy(); sHttp.err++; });
}
setInterval(pollHttp, 1000 / httpHz);

// ── WS client (raw, minimal handshake) ───────────────────────────────────
if (useWs) {
  const net = require('net');
  const crypto = require('crypto');
  const key = crypto.randomBytes(16).toString('base64');
  const ws = net.connect(80, target, () => {
    ws.write(
      `GET /ws HTTP/1.1\r\nHost: ${target}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n` +
      `Sec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`
    );
  });
  let frames = 0;
  ws.on('data', () => { frames++; });
  ws.on('error', (e) => console.error('\nws err:', e.message));
  setInterval(() => process.stdout.write(`  ws: ${frames} chunks  `), 5000);
}

// HTTP stats line
setInterval(() => process.stdout.write(`  http ok=${sHttp.n} err=${sHttp.err}`), 5000);

process.on('SIGINT', () => { sDmx.end(); sock.close(); process.exit(0); });
