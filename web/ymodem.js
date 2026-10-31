// SPDX-License-Identifier: MIT
//
// ymodem.js -- send files by YMODEM batch, as Avoidant's `rb` takes
// them (m5tab5/main/ymodem.h). Written from Forsberg's protocol reference
// for this page; no library.
//
// The port is reached through `io`, so the page and the host test
// (m5tab5/test/ymodem/send.mjs) run the same code:
//   io.read(ms)    -> a byte (0-255), or -1 if none came within ms
//   io.peek()      -> a byte if one has already arrived, else -1 (no wait)
//   io.write(u8)   -> Promise, resolved once the bytes are taken
//   io.flushIn()   -> drop whatever has arrived
//
// The receiver says which flavour it wants: 'C' for YMODEM (every block
// acknowledged), 'G' for YMODEM-G (streamed, an error ends the batch).

const SOH = 0x01, STX = 0x02, EOT = 0x04, ACK = 0x06, NAK = 0x15, CAN = 0x18, SUB = 0x1A;
const C = 0x43, G = 0x47;

export class YmodemError extends Error {}

function crc16(p, from, n) {
  let crc = 0;
  for (let i = from; i < from + n; i++) {
    crc ^= p[i] << 8;
    for (let b = 0; b < 8; b++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xFFFF : (crc << 1) & 0xFFFF;
  }
  return crc;
}

function block(seq, data, size) {
  const b = new Uint8Array(size + 5);
  b[0] = size === 1024 ? STX : SOH;
  b[1] = seq & 0xFF;
  b[2] = ~seq & 0xFF;
  b.fill(SUB, 3, 3 + size);
  b.set(data.subarray(0, size), 3);
  const crc = crc16(b, 3, size);
  b[size + 3] = crc >> 8;
  b[size + 4] = crc & 0xFF;
  return b;
}

// Block 0: "name\0size\0", in 128 bytes if it fits.
function header(name, size) {
  const t = new TextEncoder().encode(name + '\0' + size + '\0');
  const n = t.length <= 128 ? 128 : 1024;
  if (t.length > n) throw new YmodemError('file name too long: ' + name);
  const d = new Uint8Array(n);
  d.set(t);
  return block(0, d, n);
}

async function waitFor(io, wanted, ms) {
  const end = Date.now() + ms;
  for (;;) {
    const left = end - Date.now();
    if (left <= 0) return -1;
    const c = await io.read(left);
    if (c < 0) return -1;
    if (c === CAN) {
      if ((await io.read(1000)) === CAN) throw new YmodemError('cancelled by the Tab5');
      continue;
    }
    if (wanted.includes(c)) return c;
  }
}

// Send one block and get it acknowledged ('C' mode), resending on NAK.
async function sendAcked(io, b) {
  for (let tries = 0; tries < 10; tries++) {
    await io.write(b);
    const r = await waitFor(io, [ACK, NAK], 10000);
    if (r === ACK) return;
    if (r < 0) throw new YmodemError('no answer from the Tab5');
  }
  throw new YmodemError('too many bad blocks');
}

/*
 * files: [{ name, data: Uint8Array }]. name may contain '/' for a
 * subdirectory of the receiving directory.
 * onProgress(fileIndex, sent, total) as it goes.
 * Resolves with 'G' or 'C', the flavour the receiver chose; throws
 * YmodemError if the transfer did not finish.
 */
export async function ymodemSend(io, files, onProgress = () => {}) {
  let mode = await waitFor(io, [C, G], 60000);
  if (mode < 0) throw new YmodemError('the Tab5 did not ask for a YMODEM transfer');
  const streaming = mode === G;

  for (let i = 0; i < files.length; i++) {
    const { name, data } = files[i];
    // block 0, then the receiver asks again for the data
    if (streaming) await io.write(header(name, data.length));
    else await sendAcked(io, header(name, data.length));
    if ((await waitFor(io, [mode], 10000)) < 0) throw new YmodemError('the Tab5 did not take ' + name);

    onProgress(i, 0, data.length);
    let seq = 1;
    for (let off = 0; off < data.length; off += 1024, seq++) {
      const b = block(seq, data.subarray(off, off + 1024), data.length - off > 128 ? 1024 : 128);
      if (streaming) {
        // nothing comes back while streaming, but a cancel might
        if (io.peek() === CAN && (await io.read(1000)) === CAN) throw new YmodemError('cancelled by the Tab5');
        await io.write(b);
      } else {
        await sendAcked(io, b);
      }
      onProgress(i, Math.min(off + 1024, data.length), data.length);
    }

    // EOT: acknowledged, or NAKed once and then acknowledged
    let done = false;
    for (let tries = 0; tries < 10 && !done; tries++) {
      await io.write(Uint8Array.of(EOT));
      const r = await waitFor(io, [ACK, NAK], 10000);
      if (r === ACK) done = true;
      else if (r < 0) throw new YmodemError('no answer after ' + name);
    }
    if (!done) throw new YmodemError('the Tab5 did not end ' + name);
    // and it asks for the next file's block 0
    if ((await waitFor(io, [mode], 10000)) < 0) throw new YmodemError('the Tab5 stopped after ' + name);
  }

  // an empty block 0 ends the batch
  const end = block(0, new Uint8Array(128), 128);
  if (streaming) await io.write(end), await waitFor(io, [ACK], 5000);
  else await sendAcked(io, end);
  return String.fromCharCode(mode);
}
