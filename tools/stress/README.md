# Lumox Art-Net stress tests

Pure Node.js (no `npm install`). Each script is single-purpose so you can correlate `/health` numbers + the mutex-spike log with one specific load pattern.

## Run

Interactive launcher (recommended):

```bash
npm run stress           # TUI menu — pick a test, set target, run
npm run discover         # one-shot ArtPoll scan
```

Direct script invocation also works — first positional arg is the target IP, default `192.168.178.105`. All scripts honour `--target=` too:

```bash
cd tools/stress
node sustained.js 192.168.178.105 --hz=60
```

## Scripts

| Script | What it does | What to watch on `/health` |
|---|---|---|
| `sustained.js`   | Steady ArtDMX at chosen Hz, sine-sweep data | `rateHz` ≈ min(sender, 44); `stale` low |
| `burst.js`       | N back-to-back packets, sleep, repeat | `pkts` jumps in steps; mutex log |
| `stuckseq.js`    | Constant seq byte every frame | Verifies seq fix: `stale` ≈ 0 |
| `pollstorm.js`   | ArtPoll spam (50 Hz default) | Reply-path load; `--listen` counts incoming replies |
| `malformed.js`   | Bad ID / ProtVer 0 / overlong len / wrong universe | `badProto` rises; DMX output unaffected |
| `sync.js`        | ArtDMX + ArtSync pairs | `inSync=true`, `syncs` increments; `--once` for timeout test |
| `swapsim.js`     | Active/silent windows from one IP | `age` peaks during silence then resets |
| `mixed.js`       | ArtDMX + ArtPoll + HTTP + optional WS, all at once | Mutex spikes correlate with HTTP load |
| `discover.js`    | ArtPoll broadcast → list every responding node | Highlights Lumox; pre-flight before any other test |

## Real two-sender swap test

`swapsim.js` only simulates pause/resume from one source. To trigger `senderSwaps`, run two scripts simultaneously from two machines (or two NICs on the same machine — bind explicit source IPs):

```bash
# machine A
node sustained.js 192.168.178.105 --hz=44

# machine B (different LAN IP)
node sustained.js 192.168.178.105 --hz=44
```

`/health` should report `swaps > 0`, `locked > 0`, and `sender` flipping back and forth.

## Tips

- Run `curl -s http://192.168.178.105/api/status | jq .dmx,.artnet` in another terminal while a stress script runs to watch counters live.
- The mutex-spike log on `/health` keeps the last 25 events; combine with `mixed.js` to see HTTP load actually hit the DMX task.
- `Ctrl+C` stops any script cleanly.
