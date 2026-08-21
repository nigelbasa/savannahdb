// Durability contract for the two sync policies.
//
// 'batched' defers fsync to checkpoint boundaries but still fflushes every
// record, so the kernel owns committed bytes immediately. The claim under
// test: a process killed outright (no destructors, no exit handlers) loses
// nothing. That is the guarantee the 0.2.0 default rests on -- if it does not
// hold, persistence-by-default is not safe to ship.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { pathToFileURL } from 'node:url';

const sdk = pathToFileURL(path.join(process.cwd(), 'server', 'dist', 'sdk', 'index.js')).href;

function run(source, expectSignal = false) {
  const res = spawnSync(process.execPath, ['--input-type=module', '-e', source], {
    encoding: 'utf8',
  });
  if (!expectSignal && res.status !== 0) {
    throw new Error(`child failed (${res.status}): ${res.stderr}`);
  }
  return res.stdout.trim();
}

for (const sync of ['batched', 'full']) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), `savannah-dur-${sync}-`));
  const cfg = `{ backend: 'canopy', root: ${JSON.stringify(root)}, sync: '${sync}' }`;

  // Write, then die without any chance to clean up.
  const out = run(`
    import { SavannahDB } from ${JSON.stringify(sdk)};
    const c = new SavannahDB({ storage: ${cfg} }).collection('d', 'c');
    await c.insertMany([{ _id: 1, v: 'a' }, { _id: 2, v: 'b' }]);
    await c.updateOne({ _id: 1 }, { $set: { v: 'updated' } });
    process.stdout.write('written');
    // SIGKILL: no destructors, no exit handlers, no final fsync.
    process.kill(process.pid, 'SIGKILL');
  `, true);
  assert.ok(out.startsWith('written'), `${sync}: child did not reach the write`);

  const back = run(`
    import { SavannahDB } from ${JSON.stringify(sdk)};
    const c = new SavannahDB({ storage: ${cfg} }).collection('d', 'c');
    const docs = await c.find({}).sort({ _id: 1 }).toArray();
    process.stdout.write(JSON.stringify(docs.map((d) => d.v)));
  `);
  assert.equal(back, '["updated","b"]',
    `${sync}: data did not survive an uncleanly killed process`);

  fs.rmSync(root, { recursive: true, force: true });
  console.log(`  ${sync}: survived SIGKILL with no loss`);
}

// A torn tail must degrade to "lost the last record", never "cannot open".
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'savannah-torn-'));
const cfg = `{ backend: 'canopy', root: ${JSON.stringify(root)} }`;
run(`
  import { SavannahDB } from ${JSON.stringify(sdk)};
  const c = new SavannahDB({ storage: ${cfg} }).collection('d', 'c');
  await c.insertMany([{ _id: 1, v: 'a' }, { _id: 2, v: 'b' }]);
  process.stdout.write('ok');
`);

// Find the collection log and append a deliberately half-written frame.
const logs = [];
(function walk(dir) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) walk(p);
    else if (e.name.endsWith('.bin') || e.name.includes('ops')) logs.push(p);
  }
})(root);
assert.ok(logs.length > 0, 'no log file found to corrupt');
const target = logs.sort((a, b) => fs.statSync(b).size - fs.statSync(a).size)[0];
const before = fs.statSync(target).size;
// A frame claiming far more bytes than actually follow: exactly what a power
// cut mid-append leaves behind.
const torn = Buffer.alloc(9);
torn.writeUInt32LE(4096, 0);
fs.appendFileSync(target, torn);

const recovered = run(`
  import { SavannahDB } from ${JSON.stringify(sdk)};
  const c = new SavannahDB({ storage: ${cfg} }).collection('d', 'c');
  const docs = await c.find({}).sort({ _id: 1 }).toArray();
  process.stdout.write(JSON.stringify(docs.map((d) => d.v)));
`);
assert.equal(recovered, '["a","b"]', 'torn tail must not lose committed records');
assert.ok(fs.statSync(target).size <= before + 9,
  'torn tail should be truncated back, not left to grow');
console.log('  torn tail: recovered without losing committed records');

fs.rmSync(root, { recursive: true, force: true });
console.log('OK — canopy durability: batched + full survive SIGKILL, torn tail recovers');
