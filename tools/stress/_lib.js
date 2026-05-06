// Shared helpers for Lumox Art-Net stress tests.
// Pure stdlib — no npm install needed.

const dgram = require('dgram');

const ART_PORT = 6454;
const OP_POLL  = 0x2000;
const OP_DMX   = 0x5000;
const OP_SYNC  = 0x5200;
const PROT_VER = 14;

// ── Packet builders ──────────────────────────────────────────────────────
// All return Buffer ready for sendto. Field layout per Art-Net 4 spec.

function buildArtDmx({ seq = 0, physical = 0, universe = 0, data = Buffer.alloc(512) }) {
  if (data.length > 512) throw new Error('data > 512');
  const buf = Buffer.alloc(18 + data.length);
  buf.write('Art-Net\0', 0, 8, 'ascii');
  buf.writeUInt16LE(OP_DMX, 8);
  buf.writeUInt16BE(PROT_VER, 10);
  buf.writeUInt8(seq & 0xff, 12);
  buf.writeUInt8(physical & 0xff, 13);
  buf.writeUInt16LE(universe & 0x7fff, 14);
  buf.writeUInt16BE(data.length, 16);
  data.copy(buf, 18);
  return buf;
}

function buildArtPoll({ talkToMe = 0x02, diagPriority = 0x10 } = {}) {
  const buf = Buffer.alloc(14);
  buf.write('Art-Net\0', 0, 8, 'ascii');
  buf.writeUInt16LE(OP_POLL, 8);
  buf.writeUInt16BE(PROT_VER, 10);
  buf.writeUInt8(talkToMe, 12);
  buf.writeUInt8(diagPriority, 13);
  return buf;
}

function buildArtSync() {
  const buf = Buffer.alloc(14);
  buf.write('Art-Net\0', 0, 8, 'ascii');
  buf.writeUInt16LE(OP_SYNC, 8);
  buf.writeUInt16BE(PROT_VER, 10);
  return buf;
}

// ── CLI helpers ──────────────────────────────────────────────────────────

function parseArgs(argv) {
  const args = { _: [], flags: {} };
  for (let i = 2; i < argv.length; i++) {
    const a = argv[i];
    if (a.startsWith('--')) {
      const eq = a.indexOf('=');
      if (eq > 0) args.flags[a.slice(2, eq)] = a.slice(eq + 1);
      else        args.flags[a.slice(2)] = argv[++i];
    } else {
      args._.push(a);
    }
  }
  return args;
}

function getTarget(args, fallback = '192.168.178.105') {
  return args._[0] || args.flags.target || fallback;
}

function makeSocket() {
  const sock = dgram.createSocket('udp4');
  sock.bind(0);                     // ephemeral source port
  return sock;
}

// Periodic stats printer — call from any sender. Returns a tick() fn.
function statsPrinter(label) {
  let n = 0;
  let bytes = 0;
  const t0 = Date.now();
  let last = t0;
  return {
    tick(pktBytes = 0) {
      n++;
      bytes += pktBytes;
      const now = Date.now();
      if (now - last >= 1000) {
        const dt = (now - t0) / 1000;
        const pps = (n / dt).toFixed(0);
        const kbps = ((bytes * 8) / dt / 1000).toFixed(1);
        process.stdout.write(`\r[${label}] ${n} pkts · ${pps} pps · ${kbps} kbps`);
        last = now;
      }
    },
    end() { process.stdout.write('\n'); }
  };
}

function help(name, lines) {
  console.log(`Usage: node ${name} [target] [--flag=value ...]`);
  console.log(`  target defaults to 192.168.178.105 (override with first arg or --target=)`);
  if (lines) lines.forEach(l => console.log('  ' + l));
}

module.exports = {
  ART_PORT, OP_POLL, OP_DMX, OP_SYNC, PROT_VER,
  buildArtDmx, buildArtPoll, buildArtSync,
  parseArgs, getTarget, makeSocket, statsPrinter, help,
};
