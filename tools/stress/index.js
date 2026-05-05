// index.js — interactive launcher for the Lumox Art-Net stress suite.
// Uses @inquirer/prompts for a modern, accessible TUI (arrow keys, search,
// description pane). Each child stress script runs with stdio inherited so
// its output streams normally; on exit you land back in the menu.
//
//   npm run stress
//   npm run stress -- --target=10.0.0.7

const { spawn }       = require('child_process');
const path            = require('path');
const { select, input, confirm } = require('@inquirer/prompts');

const DEFAULT_TARGET = '192.168.178.105';

// ── Catalogue ────────────────────────────────────────────────────────────
// {target} is substituted with the current target before launch.
const TESTS = [
  { id: 'discover',  label: 'Discover Art-Net nodes',
    blurb: 'Broadcast ArtPoll on the LAN, list every responding node and highlight Lumox.',
    script: 'discover.js', args: '--wait=3000' },
  { id: 'sustained', label: 'Sustained ArtDMX',
    blurb: 'Steady N-Hz stream with a sine sweep — baseline throughput + rateHz check.',
    script: 'sustained.js', args: '{target} --hz=44' },
  { id: 'burst',     label: 'Burst load',
    blurb: 'N back-to-back packets per gap — stresses the UDP RX queue and pollArtNet drain.',
    script: 'burst.js', args: '{target} --count=200 --gap=500' },
  { id: 'stuckseq',  label: 'Stuck-seq (duplicate frames)',
    blurb: 'Fixed seq byte every frame — verifies the duplicate-passthrough fix.',
    script: 'stuckseq.js', args: '{target} --seq=1 --hz=44' },
  { id: 'pollstorm', label: 'ArtPoll storm',
    blurb: 'Spam OpPoll @ 50 Hz to exercise the ArtPollReply hot path.',
    script: 'pollstorm.js', args: '{target} --hz=50' },
  { id: 'malformed', label: 'Malformed packets',
    blurb: 'Bad ID / ProtVer / length / opcode — verify parser hardening.',
    script: 'malformed.js', args: '{target} --hz=50' },
  { id: 'sync',      label: 'ArtSync mode',
    blurb: 'ArtDMX + ArtSync pairs — exercises multi-node frame synchronization.',
    script: 'sync.js', args: '{target} --hz=30' },
  { id: 'swapsim',   label: 'Pause/resume sender',
    blurb: 'Active/silent windows from one IP — tests single-sender lock release.',
    script: 'swapsim.js', args: '{target} --burst=2000 --gap=3000' },
  { id: 'mixed',     label: 'Mixed concurrent load',
    blurb: 'ArtDMX + ArtPoll + HTTP polling + WS together — mutex contention test.',
    script: 'mixed.js', args: '{target} --hz=44 --http-hz=20 --ws' },
];

const ESC = '\x1b[';
const DIM    = (s) => `${ESC}2m${s}${ESC}0m`;
const BOLD   = (s) => `${ESC}1m${s}${ESC}0m`;
const ACCENT = (s) => `${ESC}38;5;75m${s}${ESC}0m`;

function parseArgv() {
  let target = DEFAULT_TARGET;
  for (const a of process.argv.slice(2)) {
    if (a.startsWith('--target=')) target = a.slice(9);
  }
  return { target };
}

function runChild(script, argString) {
  const argv = argString.length ? argString.split(/\s+/) : [];
  console.log();
  console.log(BOLD(ACCENT(`▶ ${script} ${argString}`)));
  console.log(DIM('  Ctrl+C to stop the test and return to the menu.\n'));

  return new Promise((resolve) => {
    const child = spawn(process.execPath, [path.join(__dirname, script), ...argv], {
      stdio: 'inherit',
    });
    // Forward parent SIGINT to child only — outer loop handles its own.
    let interrupted = false;
    const onSig = () => { interrupted = true; child.kill('SIGINT'); };
    process.on('SIGINT', onSig);
    child.on('close', () => {
      process.removeListener('SIGINT', onSig);
      resolve(interrupted);
    });
  });
}

async function pause() {
  await confirm({ message: 'Return to menu?', default: true });
}

async function main() {
  let target = parseArgv().target;

  console.clear();
  console.log(BOLD(ACCENT('LUMOX')) + DIM('  ·  Art-Net Stress Test Launcher'));
  console.log(DIM('  Interactive — pick a test, set parameters, run.\n'));

  for (;;) {
    const choices = [
      ...TESTS.map(t => ({
        name:  t.label,
        value: t.id,
        description: t.blurb,
      })),
      { name: '── Change target IP', value: '__target', description: `Currently: ${target}` },
      { name: '── Quit',             value: '__quit',   description: 'Exit launcher' },
    ];

    let pick;
    try {
      pick = await select({
        message: `Target ${ACCENT(target)} — what to run?`,
        choices,
        pageSize: 14,
        loop: false,
      });
    } catch (e) {
      // Ctrl+C in inquirer throws ExitPromptError
      console.log('\n' + DIM('Bye.'));
      process.exit(0);
    }

    if (pick === '__quit')   { console.log(DIM('Bye.')); return; }
    if (pick === '__target') {
      target = await input({ message: 'Target IP / hostname:', default: target });
      continue;
    }

    const test = TESTS.find(t => t.id === pick);
    const def  = test.args.replace('{target}', target);
    const args = await input({ message: 'Args:', default: def });

    await runChild(test.script, args);
    await pause();
  }
}

main().catch((e) => {
  if (e && e.name === 'ExitPromptError') process.exit(0);
  console.error(e);
  process.exit(1);
});
