// Asserts the *default* is durable: `new SavannahDB()` with no storage block
// must survive a process restart. smoke-canopy-restart.mjs sets the backend
// explicitly, so it would keep passing even if the default silently reverted
// to memory -- this is the test that pins the 0.2.0 behavior change.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { pathToFileURL } from 'node:url';

const cwd = fs.mkdtempSync(path.join(os.tmpdir(), 'savannah-default-'));
const sdk = path.join(process.cwd(), 'server', 'dist', 'sdk', 'index.js');
const sdkUrl = pathToFileURL(sdk).href;

// Run from a scratch cwd: the default root is <cwd>/.savannahdb/canopy, so
// this both exercises the real default and keeps the repo clean.
const run = (source) =>
  execFileSync(process.execPath, ['--input-type=module', '-e', source], {
    cwd,
    encoding: 'utf8',
  }).trim();

const write = run(`
  import { SavannahDB } from ${JSON.stringify(sdkUrl)};
  const db = new SavannahDB();                       // no storage config
  await db.collection('zoo', 'animals').insertMany([
    { _id: 1, species: 'lion' },
    { _id: 2, species: 'zebra' },
  ]);
  console.log('ok');
`);
assert.equal(write, 'ok');

const rootDir = path.join(cwd, '.savannahdb', 'canopy');
assert.ok(fs.existsSync(rootDir), `default root not created at ${rootDir}`);

const readBack = run(`
  import { SavannahDB } from ${JSON.stringify(sdkUrl)};
  const db = new SavannahDB();
  let docs = await db.collection('zoo', 'animals').find({});
  if (docs && typeof docs.toArray === 'function') docs = await docs.toArray();
  console.log(docs.map((d) => d.species).sort().join(','));
`);
assert.equal(readBack, 'lion,zebra', 'data did not survive restart under the default config');

// The opt-out must still be honoured, and must not touch the disk.
const ephemeral = fs.mkdtempSync(path.join(os.tmpdir(), 'savannah-memory-'));
const memRun = (source) =>
  execFileSync(process.execPath, ['--input-type=module', '-e', source], {
    cwd: ephemeral,
    encoding: 'utf8',
  }).trim();
memRun(`
  import { SavannahDB } from ${JSON.stringify(sdkUrl)};
  const db = new SavannahDB({ storage: { backend: 'memory' } });
  await db.collection('zoo', 'animals').insertMany([{ _id: 1, species: 'lion' }]);
  console.log('ok');
`);
const memBack = memRun(`
  import { SavannahDB } from ${JSON.stringify(sdkUrl)};
  const db = new SavannahDB({ storage: { backend: 'memory' } });
  let docs = await db.collection('zoo', 'animals').find({});
  if (docs && typeof docs.toArray === 'function') docs = await docs.toArray();
  console.log(String(docs.length));
`);
assert.equal(memBack, '0', 'memory backend must not persist');
assert.ok(
  !fs.existsSync(path.join(ephemeral, '.savannahdb')),
  'memory backend must not create a storage root',
);

fs.rmSync(cwd, { recursive: true, force: true });
fs.rmSync(ephemeral, { recursive: true, force: true });
console.log('OK — default persistence + explicit memory opt-out');
