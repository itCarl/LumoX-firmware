// discover.js — broadcast ArtPoll, list every Art-Net node that replies.
// Highlights Lumox nodes (shortName starts with "Lumox-" or longName contains
// "Lumox DMX"). Pure stdlib — works on any LAN interface without mDNS.
//
//   node discover.js                    (broadcast on default interface, 3 s)
//   node discover.js --bcast=255.255.255.255 --wait=5000
//   node discover.js --filter=lumox     (only show Lumox nodes)

const dgram = require('dgram');
const { ART_PORT, OP_POLL, buildArtPoll, parseArgs, help } = require('./_lib.js');

const args = parseArgs(process.argv);
if (args.flags.h || args.flags.help) {
  help('discover.js', [
    '--bcast=ADDR    broadcast address (default 255.255.255.255)',
    '--wait=MS       listen window (default 3000)',
    '--filter=lumox  show only Lumox nodes',
  ]);
  process.exit(0);
}

const bcast  = args.flags.bcast  || '255.255.255.255';
const waitMs = parseInt(args.flags.wait || '3000', 10);
const lumoxOnly = (args.flags.filter || '').toLowerCase() === 'lumox';

const OP_POLL_REPLY = 0x2100;

function parseReply(buf, from) {
  if (buf.length < 207 || buf.slice(0, 8).toString('ascii') !== 'Art-Net\0') return null;
  if (buf.readUInt16LE(8) !== OP_POLL_REPLY) return null;

  const ip      = `${buf[10]}.${buf[11]}.${buf[12]}.${buf[13]}`;
  const verHi   = buf[16];
  const verLo   = buf[17];
  const netSw   = buf[18] & 0x7f;
  const subSw   = buf[19] & 0x0f;
  const oem     = (buf[20] << 8) | buf[21];
  const esta    = buf[24] | (buf[25] << 8);
  const shortNm = buf.slice(26, 44).toString('ascii').replace(/\0.*$/, '').trim();
  const longNm  = buf.slice(44, 108).toString('ascii').replace(/\0.*$/, '').trim();
  const report  = buf.slice(108, 172).toString('ascii').replace(/\0.*$/, '').trim();
  const swOut0  = buf[190] & 0x0f;
  const universe = (netSw << 8) | (subSw << 4) | swOut0;
  const goodOut0 = buf[182];
  const mac     = Array.from(buf.slice(201, 207)).map(b => b.toString(16).padStart(2, '0')).join(':');
  const status2 = buf[212] || 0;

  return {
    from, ip,
    fw: `${verHi}.${verLo}`,
    universe,
    shortName: shortNm,
    longName:  longNm,
    nodeReport: report,
    oem: '0x' + oem.toString(16).padStart(4, '0'),
    esta: '0x' + esta.toString(16).padStart(4, '0'),
    mac,
    output: (goodOut0 & 0x80) ? 'TX' : 'idle',
    dhcp: !!(status2 & 0x02),
    artnet4: !!(status2 & 0x08),
  };
}

function isLumox(r) {
  return r.shortName.toLowerCase().startsWith('lumox') ||
         r.longName.toLowerCase().includes('lumox dmx');
}

const sock = dgram.createSocket({ type: 'udp4', reuseAddr: true });
const seen = new Map();   // ip → reply (dedupe)

sock.on('message', (msg, rinfo) => {
  const r = parseReply(msg, rinfo.address);
  if (!r) return;
  if (lumoxOnly && !isLumox(r)) return;
  seen.set(r.ip, r);
});

sock.on('error', (e) => { console.error('socket error:', e.message); process.exit(1); });

sock.bind(ART_PORT, () => {
  sock.setBroadcast(true);
  const poll = buildArtPoll();
  sock.send(poll, ART_PORT, bcast);
  console.log(`ArtPoll → ${bcast}:${ART_PORT}  (waiting ${waitMs} ms${lumoxOnly ? ', Lumox only' : ''})`);
  setTimeout(report, waitMs);
});

function report() {
  if (seen.size === 0) {
    console.log('\nNo nodes responded.');
    if (lumoxOnly) console.log('  (Lumox filter active — drop --filter=lumox to see all Art-Net nodes.)');
    sock.close(); process.exit(2);
  }

  const sorted = [...seen.values()].sort((a, b) => a.ip.localeCompare(b.ip, 'en'));
  console.log(`\nFound ${sorted.length} node${sorted.length > 1 ? 's' : ''}:\n`);
  for (const r of sorted) {
    const tag = isLumox(r) ? '\x1b[36m[LUMOX]\x1b[0m ' : '        ';
    console.log(`${tag}${r.ip.padEnd(16)} uni=${String(r.universe).padEnd(5)} fw=${r.fw}  ${r.shortName}`);
    console.log(`         long: ${r.longName}`);
    console.log(`         mac:  ${r.mac}   oem=${r.oem}   esta=${r.esta}   ${r.output}${r.dhcp ? ' dhcp' : ''}${r.artnet4 ? ' an4' : ''}`);
    if (r.nodeReport) console.log(`         report: ${r.nodeReport}`);
    console.log();
  }

  const lumox = sorted.filter(isLumox);
  if (lumox.length) {
    console.log(`\x1b[36mLumox node${lumox.length > 1 ? 's' : ''} detected:\x1b[0m ${lumox.map(r => r.ip).join(', ')}`);
    process.exit(0);
  } else {
    console.log('No Lumox node on this LAN segment.');
    process.exit(3);
  }
}

process.on('SIGINT', () => { sock.close(); process.exit(130); });
