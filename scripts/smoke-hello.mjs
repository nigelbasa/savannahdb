// Tiny smoke test: opens a TCP socket to port 27017, sends an OP_MSG
// `hello` command, prints the decoded response, and exits.

import net from 'node:net';
import { BSON } from 'bson';

const OP_MSG = 2013;

function encodeHello(requestId) {
  const body = Buffer.from(
    BSON.serialize({
      hello: 1,
      $db: 'admin',
      client: { application: { name: 'savannah-smoke' } },
    }),
  );
  const total = 16 + 4 + 1 + body.length;
  const out = Buffer.allocUnsafe(total);
  out.writeInt32LE(total, 0);
  out.writeInt32LE(requestId, 4);
  out.writeInt32LE(0, 8);
  out.writeInt32LE(OP_MSG, 12);
  out.writeUInt32LE(0, 16);
  out.writeUInt8(0, 20);
  body.copy(out, 21);
  return out;
}

function decodeFrame(buf) {
  const flagBits = buf.readUInt32LE(16);
  const kind = buf.readUInt8(20);
  if (kind !== 0) throw new Error(`unexpected kind ${kind}`);
  const docLen = buf.readInt32LE(21);
  const body = buf.subarray(21, 21 + docLen);
  return { flagBits, doc: BSON.deserialize(body) };
}

const sock = net.connect(27017, '127.0.0.1');
let buf = Buffer.alloc(0);
const timer = setTimeout(() => {
  console.error('timeout');
  process.exit(2);
}, 3000);

sock.on('connect', () => sock.write(encodeHello(1)));
sock.on('data', (c) => {
  buf = Buffer.concat([buf, c]);
  if (buf.length < 4) return;
  const total = buf.readInt32LE(0);
  if (buf.length < total) return;
  clearTimeout(timer);
  const frame = buf.subarray(0, total);
  const { flagBits, doc } = decodeFrame(frame);
  console.log('flagBits:', flagBits);
  console.log('response:', JSON.stringify(doc, null, 2));
  sock.end();
  process.exit(0);
});
sock.on('error', (e) => {
  console.error('socket error:', e.message);
  process.exit(1);
});
