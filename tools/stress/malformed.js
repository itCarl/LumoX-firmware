// malformed.js — fire packets the parser MUST reject. Verifies hardening:
//   bad ID, ProtVer < 14, dataLen mismatch, truncated headers, wrong universe.
// /api/status should show artnet.badProto / locked / stale rising as expected
// while DMX output stays clean (last valid frame held).
//
//   node malformed.js [target] [--hz=50]

const { ART_PORT, OP_DMX, parseArgs, getTarget, makeSocket, statsPrinter, help }
  = require('./_lib.js');

const args = parseArgs(process.argv);
if (args.flags.h || args.flags.help) {
  help('malformed.js', [
    '--hz=N      packets per second across all variants (default 50)',
  ]);
  process.exit(0);
}

const target = getTarget(args);
const hz     = parseInt(args.flags.hz || '50', 10);

const sock  = makeSocket();
const stats = statsPrinter(`malformed ${hz}Hz`);

// Pre-build malformed variants. Cycle through them.
const variants = [];

// 1. Bad ID string
{ const b = Buffer.alloc(18); b.write('NotArt!\0', 0, 8, 'ascii');
  b.writeUInt16LE(OP_DMX, 8); b.writeUInt16BE(14, 10);
  variants.push({ name: 'bad-id', buf: b }); }

// 2. ProtVer 0
{ const b = Buffer.alloc(18 + 4); b.write('Art-Net\0', 0, 8, 'ascii');
  b.writeUInt16LE(OP_DMX, 8); b.writeUInt16BE(0, 10);
  b.writeUInt16BE(4, 16); b.fill(0xff, 18);
  variants.push({ name: 'protver-0', buf: b }); }

// 3. dataLen claims 600 (> 512)
{ const b = Buffer.alloc(18 + 4); b.write('Art-Net\0', 0, 8, 'ascii');
  b.writeUInt16LE(OP_DMX, 8); b.writeUInt16BE(14, 10);
  b.writeUInt16BE(600, 16);
  variants.push({ name: 'overlong-len', buf: b }); }

// 4. Truncated header (only 6 bytes)
variants.push({ name: 'tiny', buf: Buffer.from('Art-N\0') });

// 5. Wrong universe — sent by another sender first to test single-sender lock
{ const b = Buffer.alloc(18 + 8); b.write('Art-Net\0', 0, 8, 'ascii');
  b.writeUInt16LE(OP_DMX, 8); b.writeUInt16BE(14, 10);
  b.writeUInt8(1, 12); b.writeUInt16LE(99, 14); b.writeUInt16BE(8, 16);
  variants.push({ name: 'wrong-uni-99', buf: b }); }

// 6. Random opcode
{ const b = Buffer.alloc(14); b.write('Art-Net\0', 0, 8, 'ascii');
  b.writeUInt16LE(0xdead, 8); b.writeUInt16BE(14, 10);
  variants.push({ name: 'random-opcode', buf: b }); }

console.log(`→ ${target}:${ART_PORT}  cycling ${variants.length} malformed variants @ ${hz}Hz`);
variants.forEach(v => console.log(`  • ${v.name} (${v.buf.length} B)`));

let i = 0;
setInterval(() => {
  const v = variants[i++ % variants.length];
  sock.send(v.buf, ART_PORT, target);
  stats.tick(v.buf.length);
}, 1000 / hz);

process.on('SIGINT', () => { stats.end(); sock.close(); process.exit(0); });
