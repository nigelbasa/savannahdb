// Audit runner: executes the case set against SavannahDB (embedded) and
// MongoDB, diffs results, measures throughput, and emits a JSON report.
//
// Usage:
//   node scripts/audit/run.mjs
//   MONGODB_URI=mongodb://localhost:27017 node scripts/audit/run.mjs
//   node scripts/audit/run.mjs --skip-mongo  (accuracy section omitted)
//
// Outputs artifacts/audit-report.json which scripts/audit/render.mjs reads.

import { mkdirSync, writeFileSync, rmSync, existsSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { isDeepStrictEqual, inspect } from 'node:util';
import { performance } from 'node:perf_hooks';

import { SavannahDB } from '@nigelbasa/savannahdb';
import { cases } from './cases.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, '..', '..');
const OUT_FILE = resolve(repoRoot, 'artifacts', 'audit-report.json');
mkdirSync(dirname(OUT_FILE), { recursive: true });

const MONGODB_URI = process.env.MONGODB_URI || 'mongodb://127.0.0.1:27017';
const SKIP_MONGO = process.argv.includes('--skip-mongo');

// ----------------------------------------------------------------------------
// MongoDB client — optional. If unreachable, we run SavannahDB-only and mark
// every accuracy result as "mongo-unavailable" rather than failing the run.
// ----------------------------------------------------------------------------

let MongoClient = null;
let mongoClient = null;
let mongoReachable = false;
if (!SKIP_MONGO) {
  try {
    ({ MongoClient } = await import('mongodb'));
    mongoClient = new MongoClient(MONGODB_URI, {
      serverSelectionTimeoutMS: 2000,
      directConnection: true,
    });
    await mongoClient.connect();
    await mongoClient.db('audit_probe').command({ ping: 1 });
    mongoReachable = true;
  } catch (err) {
    mongoReachable = false;
    console.warn(`MongoDB unavailable at ${MONGODB_URI} (${err.message}). Accuracy diffs skipped.`);
  }
}

// ----------------------------------------------------------------------------
// Per-case shim. Each side gets a "collection map" exposing the same API the
// case author wrote against.
// ----------------------------------------------------------------------------

function savannahCollections(db, fixtureKeys, dbName) {
  const map = {};
  for (const key of fixtureKeys) {
    map[key] = db.collection(dbName, key);
  }
  return map;
}

async function mongoCollections(client, fixtureKeys, dbName) {
  const mdb = client.db(dbName);
  // Drop any leftover collections from a prior run before loading fixtures.
  await Promise.all(
    fixtureKeys.map(k => mdb.collection(k).deleteMany({}).catch(() => {})),
  );
  const map = {};
  for (const key of fixtureKeys) {
    map[key] = mdb.collection(key);
  }
  return map;
}

async function loadFixture(coll, docs) {
  if (docs.length === 0) return;
  await coll.insertMany(docs.map(d => structuredClone(d)));
}

// Normalize MongoDB types that SavannahDB returns as plain JS numbers / strings
// — Long → number, Decimal128 → number when finite, ObjectId → string. We
// only collapse types that the SDK explicitly returns as plain JS values.
function normalize(value) {
  if (value === null || value === undefined) return value;
  if (Array.isArray(value)) return value.map(normalize);
  if (typeof value === 'object') {
    if (typeof value.toNumber === 'function' && (value._bsontype === 'Long' || value._bsontype === 'Int32')) {
      return value.toNumber();
    }
    if (value._bsontype === 'Double') {
      return value.value;
    }
    if (value._bsontype === 'Decimal128') {
      return Number(value.toString());
    }
    if (value._bsontype === 'ObjectId') {
      return value.toString();
    }
    const out = {};
    for (const [k, v] of Object.entries(value)) out[k] = normalize(v);
    return out;
  }
  return value;
}

// ----------------------------------------------------------------------------
// Accuracy run
// ----------------------------------------------------------------------------

const accuracyResults = [];

const savannah = new SavannahDB(); // embedded, memory backend

let caseIdx = 0;
for (const tc of cases) {
  caseIdx += 1;
  const dbName = `audit_${caseIdx}`;
  const fixtureKeys = Object.keys(tc.fixture);

  // Load fixtures into SavannahDB
  const savColls = savannahCollections(savannah, fixtureKeys, dbName);
  for (const key of fixtureKeys) {
    await loadFixture(savColls[key], tc.fixture[key]);
  }

  let savannahResult, savannahError;
  const t0 = performance.now();
  try {
    savannahResult = await tc.run(savColls);
  } catch (err) {
    savannahError = err.message;
  }
  const savannahMs = performance.now() - t0;

  let mongoResult, mongoError, mongoMs = null;
  if (mongoReachable) {
    const mongoColls = await mongoCollections(mongoClient, fixtureKeys, dbName);
    for (const key of fixtureKeys) {
      await loadFixture(mongoColls[key], tc.fixture[key]);
    }
    const t1 = performance.now();
    try {
      mongoResult = await tc.run(mongoColls);
    } catch (err) {
      mongoError = err.message;
    }
    mongoMs = performance.now() - t1;
  }

  let verdict = 'pending';
  let diff = null;
  if (savannahError) {
    verdict = 'savannah-error';
    diff = savannahError;
  } else if (!mongoReachable) {
    verdict = 'mongo-unavailable';
  } else if (mongoError) {
    verdict = 'mongo-error';
    diff = mongoError;
  } else {
    const a = normalize(savannahResult);
    const b = normalize(mongoResult);
    if (isDeepStrictEqual(a, b)) {
      verdict = 'match';
    } else {
      verdict = 'diverge';
      diff = `savannah: ${inspect(a, { depth: 6, breakLength: 100 })}\nmongo:    ${inspect(b, { depth: 6, breakLength: 100 })}`;
    }
  }

  accuracyResults.push({
    id: tc.id,
    category: tc.category,
    operator: tc.operator,
    verdict,
    savannahMs: +savannahMs.toFixed(2),
    mongoMs: mongoMs !== null ? +mongoMs.toFixed(2) : null,
    diff,
  });

  process.stdout.write(`  [${verdict.padEnd(18)}] ${tc.id}\n`);
}

// ----------------------------------------------------------------------------
// Throughput micro-benchmarks. Embedded SavannahDB only (no network hop
// would make MongoDB look unfairly slow at this scale).
// ----------------------------------------------------------------------------

const perfResults = [];

async function bench(label, op, iterations) {
  // Warmup
  for (let i = 0; i < Math.min(50, iterations); i++) await op(i);
  const t = performance.now();
  for (let i = 0; i < iterations; i++) await op(i);
  const elapsed = performance.now() - t;
  const opsPerSec = (iterations / (elapsed / 1000)).toFixed(0);
  perfResults.push({ label, iterations, totalMs: +elapsed.toFixed(1), opsPerSec: +opsPerSec });
  process.stdout.write(`  ${label.padEnd(28)} ${iterations.toString().padStart(6)} ops in ${elapsed.toFixed(0).padStart(5)} ms  →  ${opsPerSec.padStart(7)} ops/s\n`);
}

const perfDb = new SavannahDB();
const perfColl = perfDb.collection('perf', 'docs');

await bench('insertOne (cold)', async i => {
  await perfColl.insertOne({ _id: i, name: `doc-${i}`, val: i });
}, 5000);

// Build an index on `val` and re-bench query throughput
await perfColl.createIndex('val_idx', 'val');

await bench('findOne(_id) — pk', async i => {
  await perfColl.findOne({ _id: i });
}, 5000);

await bench('findOne(val) — idx', async i => {
  await perfColl.findOne({ val: i });
}, 5000);

await bench('find range (val gt)', async i => {
  await perfColl.find({ val: { $gt: i, $lt: i + 50 } }).toArray();
}, 1000);

await bench('aggregate $group sum', async () => {
  await perfColl
    .aggregate([{ $group: { _id: null, total: { $sum: '$val' } } }])
    .toArray();
}, 200);

await bench('updateOne $inc', async i => {
  await perfColl.updateOne({ _id: i % 1000 }, { $inc: { val: 1 } });
}, 2000);

// ----------------------------------------------------------------------------
// Persistence (Canopy backend) — insert, close, reopen, verify.
// ----------------------------------------------------------------------------

const persistResults = [];

const canopyRoot = resolve(repoRoot, '.savannahdb-audit-canopy');
if (existsSync(canopyRoot)) rmSync(canopyRoot, { recursive: true, force: true });

// SavannahDB resolves backend choice from env at first engine load. Since
// the embedded engine is a singleton inside the addon process, we need a
// fresh Node process to flip from memory → canopy. The clean way is to spawn.

import { spawnSync } from 'node:child_process';

function spawnNode(script) {
  const res = spawnSync(process.execPath, ['-e', script], {
    encoding: 'utf8',
    env: { ...process.env, SAVANNAH_STORAGE_BACKEND: 'canopy', SAVANNAH_STORAGE_ROOT: canopyRoot },
  });
  if (res.status !== 0) {
    throw new Error(`spawn failed (status ${res.status}): ${res.stderr || res.stdout}`);
  }
  return res.stdout.trim();
}

try {
  // Process 1: insert
  const insertScript = `
    import { SavannahDB } from '${pathToImport(resolve(repoRoot, 'server', 'dist', 'index.js'))}';
    const db = new SavannahDB();
    const c = db.collection('persist', 'docs');
    await c.insertMany([{ _id: 1, n: 'first' }, { _id: 2, n: 'second' }, { _id: 3, n: 'third' }]);
    await c.createIndex('n_idx', 'n');
    process.stdout.write('inserted');
  `;
  const ins = spawnNode(insertScript);
  persistResults.push({ step: 'insert-then-exit', ok: ins === 'inserted', detail: ins });

  // Process 2: reopen, query
  const reopenScript = `
    import { SavannahDB } from '${pathToImport(resolve(repoRoot, 'server', 'dist', 'index.js'))}';
    const db = new SavannahDB();
    const c = db.collection('persist', 'docs');
    const all = await c.find({}).sort({ _id: 1 }).toArray();
    const indexed = await c.find({ n: 'second' }).toArray();
    const idxList = await c.listIndexes();
    process.stdout.write(JSON.stringify({
      total: all.length,
      ids: all.map(d => d._id),
      indexedHit: indexed.length === 1 && indexed[0]._id === 2,
      indexes: idxList.map(i => ({ name: i.name, fieldPath: i.fieldPath })),
    }));
  `;
  const reopenOut = spawnNode(reopenScript);
  const parsed = JSON.parse(reopenOut);
  persistResults.push({
    step: 'reopen-read',
    ok: parsed.total === 3 && parsed.ids.join(',') === '1,2,3' && parsed.indexedHit,
    detail: parsed,
  });

  // Process 3: delete + reopen, verify deletion survived
  const deleteScript = `
    import { SavannahDB } from '${pathToImport(resolve(repoRoot, 'server', 'dist', 'index.js'))}';
    const db = new SavannahDB();
    const c = db.collection('persist', 'docs');
    await c.deleteOne({ _id: 2 });
    process.stdout.write('deleted');
  `;
  spawnNode(deleteScript);
  const recheckOut = spawnNode(reopenScript);
  const reparsed = JSON.parse(recheckOut);
  persistResults.push({
    step: 'delete-survives-restart',
    ok: reparsed.total === 2 && !reparsed.ids.includes(2),
    detail: reparsed,
  });

  for (const r of persistResults) {
    process.stdout.write(`  ${r.ok ? '✓' : '✗'} ${r.step}\n`);
  }
} finally {
  if (existsSync(canopyRoot)) {
    try { rmSync(canopyRoot, { recursive: true, force: true }); } catch {}
  }
}

function pathToImport(p) {
  return 'file:///' + p.replace(/\\/g, '/');
}

// ----------------------------------------------------------------------------
// Emit report
// ----------------------------------------------------------------------------

const report = {
  generatedAt: new Date().toISOString(),
  node: process.version,
  platform: `${process.platform}-${process.arch}`,
  mongo: mongoReachable
    ? await mongoClient.db('admin').command({ buildInfo: 1 }).then(b => ({ version: b.version })).catch(() => null)
    : { available: false, uri: MONGODB_URI },
  accuracy: accuracyResults,
  perf: perfResults,
  persistence: persistResults,
};

writeFileSync(OUT_FILE, JSON.stringify(report, null, 2));
console.log(`\nReport written: ${OUT_FILE}`);

if (mongoClient) await mongoClient.close();
