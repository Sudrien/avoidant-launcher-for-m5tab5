// SPDX-License-Identifier: MIT
//
// send.mjs -- web/ymodem.js, the sender the web flasher runs, against
// main/ymodem.c on the host (rx_host). Node 18 or later.
//
//   make && node send.mjs
import { spawn } from 'node:child_process';
import { mkdtempSync, mkdirSync, readFileSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { randomBytes } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { ymodemSend, YmodemError } from '../../../web/ymodem.js';

const here = dirname(fileURLToPath(import.meta.url));

// rx_host on pipes, behind the io the page gives ymodemSend()
function receiver(args) {
  const p = spawn(join(here, 'rx_host'), args, { stdio: ['pipe', 'pipe', 'pipe'] });
  const q = [];
  let wake = null, log = '';
  p.stdout.on('data', d => { q.push(...d); if (wake) { wake(); wake = null; } });
  p.stderr.on('data', d => { log += d; });
  const exited = new Promise(r => p.on('exit', code => r(code)));
  const io = {
    async read(ms) {
      const end = Date.now() + ms;
      while (!q.length) {
        const left = end - Date.now();
        if (left <= 0) return -1;
        await new Promise(r => { wake = r; setTimeout(r, left); });
      }
      return q.shift();
    },
    peek: () => (q.length ? q[0] : -1),
    write: u8 => new Promise(r => (p.stdin.write(u8) ? r() : p.stdin.once('drain', r))),
    flushIn: () => { q.length = 0; },
  };
  return { io, exited, log: () => log };
}

let fail = 0;
const ok = (what, cond) => { console.log((cond ? 'ok   ' : 'FAIL ') + what); if (!cond) fail = 1; };

const files = [
  { name: 'big.bin', data: randomBytes(1048577) },
  { name: 'small.bin', data: randomBytes(1000) },
  { name: 'tiny.bin', data: randomBytes(100) },
  { name: 'empty.bin', data: new Uint8Array(0) },
  { name: 'sub/dir/app.bin', data: randomBytes(70000) },
];

for (const flags of [[], ['-g']]) {
  const tag = flags.length ? '-g' : '(C)';
  const out = mkdtempSync(join(tmpdir(), 'ym-'));
  const rx = receiver([...flags, out]);
  let err = null, mode = null;
  const t0 = Date.now();
  try { mode = await ymodemSend(rx.io, files); } catch (e) { err = e; }
  const code = await rx.exited;
  ok(`batch ${tag}: sent as ${mode}, receiver exit ${code}, ${Date.now() - t0} ms${err ? ' -- ' + err.message : ''}`, !err && code === 0);
  for (const f of files) {
    const p = join(out, f.name);
    ok(`  ${f.name} ${tag}`, existsSync(p) && Buffer.compare(readFileSync(p), Buffer.from(f.data)) === 0);
  }

  // again without -f: refused, and the old file left alone
  const again = receiver([...flags, out]);
  let refused = null;
  try { await ymodemSend(again.io, [{ name: 'small.bin', data: randomBytes(10) }]); } catch (e) { refused = e; }
  await again.exited;
  ok(`  refused to replace without -f ${tag}: ${refused && refused.message}`, refused instanceof YmodemError);
  ok(`  and small.bin is as it was ${tag}`, Buffer.compare(readFileSync(join(out, 'small.bin')), Buffer.from(files[1].data)) === 0);

  // with -f: replaced
  const newer = randomBytes(5000);
  const force = receiver(['-f', ...flags, out]);
  let ferr = null;
  try { await ymodemSend(force.io, [{ name: 'small.bin', data: newer }]); } catch (e) { ferr = e; }
  await force.exited;
  ok(`  replaced with -f ${tag}`, !ferr && Buffer.compare(readFileSync(join(out, 'small.bin')), newer) === 0);

  // a name trying to leave the directory: refused
  const esc = receiver([...flags, out]);
  let eerr = null;
  try { await ymodemSend(esc.io, [{ name: '../escape.bin', data: randomBytes(10) }]); } catch (e) { eerr = e; }
  await esc.exited;
  ok(`  refused ../escape.bin ${tag}`, eerr instanceof YmodemError && !existsSync(join(out, '..', 'escape.bin')));
}
process.exit(fail);
