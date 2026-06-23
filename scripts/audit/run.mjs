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
// Comparative perf — same workloads, SavannahDB vs SQLite (better-sqlite3,
// in-process). SQLite is the right comparator for an embedded doc DB: same
// no-network paradigm, well-tuned C engine, predictable numbers. We include
// the comparison only when better-sqlite3 is installed; CI installs it, so
// the bench workflow always populates these rows. Local dev runs without it
// just skip the section.
//
// Workloads cover both small-N micro ops (to catch per-call overhead) and
// larger N=50,000 sweeps (to expose index/scan scaling that 5K hides).
// ----------------------------------------------------------------------------

const comparativeResults = [];

let Database = null;
try {
  ({ default: Database } = await import('better-sqlite3'));
} catch (err) {
  // Loud, not silent. The comparator is the headline of this report — a
  // missing better-sqlite3 in CI usually means a missed --ignore-scripts or
  // an unsupported Node version; we want that to be obvious, not buried.
  console.warn('=== better-sqlite3 unavailable, comparative perf SKIPPED ===');
  console.warn(`reason: ${err.message}`);
  if (process.env.CI === 'true') {
    console.warn('(failing this in CI: set BENCH_ALLOW_NO_SQLITE=1 to bypass)');
    if (!process.env.BENCH_ALLOW_NO_SQLITE) {
      process.exitCode = 2;
    }
  }
}

if (Database) {
  // Each comparative workload runs both engines from a known-clean state with
  // identical document shapes. We avoid any prepared-statement reuse advantage
  // for SQLite that the SavannahDB SDK can't match — call the SDK API the same
  // way users would.
  async function compare(label, scale, savFn, sqliteFn, opts = {}) {
    // Reads benefit from cache/branch-predictor warmup; destructive ops
    // (insert) would collide with the main loop on the same ids and crash
    // SQLite's UNIQUE constraint, so warmup is skipped for those.
    if (opts.warmup !== false) {
      for (let i = 0; i < Math.min(50, scale.iter); i++) await savFn(i);
      for (let i = 0; i < Math.min(50, scale.iter); i++) sqliteFn(i);
    }

    const tSav = performance.now();
    for (let i = 0; i < scale.iter; i++) await savFn(i);
    const savElapsed = performance.now() - tSav;

    const tSql = performance.now();
    for (let i = 0; i < scale.iter; i++) sqliteFn(i);
    const sqlElapsed = performance.now() - tSql;

    const savOps = +(scale.iter / (savElapsed / 1000)).toFixed(0);
    const sqlOps = +(scale.iter / (sqlElapsed / 1000)).toFixed(0);
    const ratio = +(savOps / sqlOps).toFixed(2);

    comparativeResults.push({
      label,
      iterations: scale.iter,
      savannahOpsPerSec: savOps,
      sqliteOpsPerSec: sqlOps,
      ratio,
    });

    const marker = ratio >= 0.9 ? '✓' : (ratio >= 0.5 ? '·' : '!');
    process.stdout.write(
      `  ${marker} ${label.padEnd(34)} sav ${savOps.toLocaleString().padStart(8)}/s  ` +
      `sqlite ${sqlOps.toLocaleString().padStart(8)}/s  ratio ${ratio.toFixed(2)}\n`
    );
  }

  // -- Small-N point ops (per-call overhead) ---------------------------------
  {
    const sav = new SavannahDB().collection('cmp', 'small');
    const sqlite = new Database(':memory:');
    sqlite.exec('CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, val INTEGER)');
    const insertStmt = sqlite.prepare('INSERT INTO t (id, name, val) VALUES (?, ?, ?)');
    const N = 5000;

    // Both engines use the same id range [0, N) so subsequent read workloads
    // exercise identical key cardinality. Insert workload skips warmup so the
    // main loop doesn't collide with already-inserted ids.
    await compare('insertOne', { iter: N },
      async i => { await sav.insertOne({ _id: i, name: `doc-${i}`, val: i }); },
      i => { insertStmt.run(i, `doc-${i}`, i); },
      { warmup: false },
    );

    // Build secondary indexes on both sides so subsequent reads are fair.
    await sav.createIndex('val_idx', 'val');
    sqlite.exec('CREATE INDEX val_idx ON t(val)');

    const findByPk = sqlite.prepare('SELECT * FROM t WHERE id = ?');
    await compare('findOne(_id) — pk', { iter: N },
      async i => { await sav.findOne({ _id: i }); },
      i => { findByPk.get(i); },
    );

    const findByVal = sqlite.prepare('SELECT * FROM t WHERE val = ?');
    await compare('findOne(val) — idx', { iter: N },
      async i => { await sav.findOne({ val: i }); },
      i => { findByVal.get(i); },
    );

    const updateById = sqlite.prepare('UPDATE t SET val = val + 1 WHERE id = ?');
    await compare('updateOne $inc', { iter: 2000 },
      async i => { await sav.updateOne({ _id: i % 1000 }, { $inc: { val: 1 } }); },
      i => { updateById.run(i % 1000); },
    );

    sqlite.close();
  }

  // -- Index-stress workloads at scale (50K docs) ----------------------------
  // These expose what the 5K micro-ops hide: index maintenance during bulk
  // load, range-scan selectivity, and aggregation cost over a large set.
  {
    const SCALE = 50_000;

    // (1) Bulk-insert with implicit _id index only (no secondary).
    //     Measures pure insert path + _id maintenance.
    const savBulk = new SavannahDB().collection('cmp', 'bulk_idx_id_only');
    const sqliteBulk = new Database(':memory:');
    sqliteBulk.exec('CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, val INTEGER)');
    const sqlInsertBulk = sqliteBulk.prepare('INSERT INTO t (id, name, val) VALUES (?, ?, ?)');
    const sqlInsertBulkTx = sqliteBulk.transaction(rows => {
      for (const r of rows) sqlInsertBulk.run(r.id, r.name, r.val);
    });

    const bulkRows = Array.from({ length: SCALE }, (_, i) => ({
      id: i, _id: i, name: `doc-${i}`, val: (i * 7919) % SCALE,
    }));

    const tSavBulk = performance.now();
    // Chunk SavannahDB inserts; the SDK's insertMany already takes arrays.
    for (let i = 0; i < SCALE; i += 1000) {
      await savBulk.insertMany(bulkRows.slice(i, i + 1000));
    }
    const savBulkMs = performance.now() - tSavBulk;

    const tSqlBulk = performance.now();
    sqlInsertBulkTx(bulkRows);
    const sqlBulkMs = performance.now() - tSqlBulk;

    const savBulkOps = +(SCALE / (savBulkMs / 1000)).toFixed(0);
    const sqlBulkOps = +(SCALE / (sqlBulkMs / 1000)).toFixed(0);
    comparativeResults.push({
      label: `bulk insert (${SCALE.toLocaleString()} docs, _id only)`,
      iterations: SCALE,
      savannahOpsPerSec: savBulkOps,
      sqliteOpsPerSec: sqlBulkOps,
      ratio: +(savBulkOps / sqlBulkOps).toFixed(2),
    });
    process.stdout.write(
      `  · bulk insert ${SCALE} docs (_id only)     sav ${savBulkOps.toLocaleString()}/s  sqlite ${sqlBulkOps.toLocaleString()}/s\n`
    );

    // (1b) Bulk-insert with secondary index ALREADY present — this is what
    //      the AUDIT.md note about "_id index costs ~30%" is really asking:
    //      how much does maintaining a non-PK index on every write cost?
    //      The ratio against (1) is the index-write tax.
    const savBulkIdx = new SavannahDB().collection('cmp', 'bulk_with_secondary');
    await savBulkIdx.createIndex('val_idx', 'val');
    const sqliteBulkIdx = new Database(':memory:');
    sqliteBulkIdx.exec('CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, val INTEGER)');
    sqliteBulkIdx.exec('CREATE INDEX val_idx ON t(val)');
    const sqlInsertIdx = sqliteBulkIdx.prepare('INSERT INTO t (id, name, val) VALUES (?, ?, ?)');
    const sqlInsertIdxTx = sqliteBulkIdx.transaction(rows => {
      for (const r of rows) sqlInsertIdx.run(r._id, r.name, r.val);
    });

    const tSavBulkIdx = performance.now();
    for (let i = 0; i < SCALE; i += 1000) {
      await savBulkIdx.insertMany(bulkRows.slice(i, i + 1000));
    }
    const savBulkIdxMs = performance.now() - tSavBulkIdx;

    const tSqlBulkIdx = performance.now();
    sqlInsertIdxTx(bulkRows);
    const sqlBulkIdxMs = performance.now() - tSqlBulkIdx;

    const savBulkIdxOps = +(SCALE / (savBulkIdxMs / 1000)).toFixed(0);
    const sqlBulkIdxOps = +(SCALE / (sqlBulkIdxMs / 1000)).toFixed(0);
    comparativeResults.push({
      label: `bulk insert (${SCALE.toLocaleString()} docs, +secondary idx)`,
      iterations: SCALE,
      savannahOpsPerSec: savBulkIdxOps,
      sqliteOpsPerSec: sqlBulkIdxOps,
      ratio: +(savBulkIdxOps / sqlBulkIdxOps).toFixed(2),
    });
    const indexWriteTax = (1 - savBulkIdxOps / savBulkOps) * 100;
    process.stdout.write(
      `  · bulk insert ${SCALE} docs (+sec idx)     sav ${savBulkIdxOps.toLocaleString()}/s  sqlite ${sqlBulkIdxOps.toLocaleString()}/s  ` +
      `(index-write tax: ${indexWriteTax.toFixed(0)}%)\n`
    );
    sqliteBulkIdx.close();

    // (2) Build secondary index over the loaded set (one-shot indexing speed).
    const tSavIdx = performance.now();
    await savBulk.createIndex('val_idx', 'val');
    const savIdxMs = +(performance.now() - tSavIdx).toFixed(1);

    const tSqlIdx = performance.now();
    sqliteBulk.exec('CREATE INDEX val_idx ON t(val)');
    const sqlIdxMs = +(performance.now() - tSqlIdx).toFixed(1);

    comparativeResults.push({
      label: `build secondary index over ${SCALE.toLocaleString()} docs`,
      iterations: 1,
      savannahOpsPerSec: +(1000 / savIdxMs).toFixed(2),
      sqliteOpsPerSec: +(1000 / sqlIdxMs).toFixed(2),
      ratio: +(sqlIdxMs / savIdxMs).toFixed(2), // higher = SavannahDB faster
      savannahMs: savIdxMs,
      sqliteMs: sqlIdxMs,
      kind: 'oneshot',
    });
    process.stdout.write(
      `  · build secondary index                   sav ${savIdxMs} ms        sqlite ${sqlIdxMs} ms\n`
    );

    // (3) Range scan with idx — 1K queries, each returning ~50 docs.
    const sqlRange = sqliteBulk.prepare('SELECT * FROM t WHERE val > ? AND val < ?');
    const tSavRange = performance.now();
    for (let i = 0; i < 1000; i++) {
      await savBulk.find({ val: { $gt: i, $lt: i + 50 } }).toArray();
    }
    const savRangeMs = performance.now() - tSavRange;
    const tSqlRange = performance.now();
    for (let i = 0; i < 1000; i++) {
      sqlRange.all(i, i + 50);
    }
    const sqlRangeMs = performance.now() - tSqlRange;
    const savRangeOps = +(1000 / (savRangeMs / 1000)).toFixed(0);
    const sqlRangeOps = +(1000 / (sqlRangeMs / 1000)).toFixed(0);
    comparativeResults.push({
      label: `range scan (val gt, ${SCALE.toLocaleString()} docs)`,
      iterations: 1000,
      savannahOpsPerSec: savRangeOps,
      sqliteOpsPerSec: sqlRangeOps,
      ratio: +(savRangeOps / sqlRangeOps).toFixed(2),
    });
    process.stdout.write(
      `  · range scan over ${SCALE} docs           sav ${savRangeOps.toLocaleString()}/s  sqlite ${sqlRangeOps.toLocaleString()}/s\n`
    );

    // (4) Aggregation at scale: SUM(val) over 50K docs, 100 iterations.
    //     This is the workload most likely to expose the pipeline hot path
    //     (AUDIT.md flagged 290 ops/s at 5K — at 50K we should see whether
    //     the slowdown is linear-in-N or worse).
    const sqlSum = sqliteBulk.prepare('SELECT SUM(val) as total FROM t');
    const ITER_AGG = 100;
    const tSavAgg = performance.now();
    for (let i = 0; i < ITER_AGG; i++) {
      await savBulk.aggregate([{ $group: { _id: null, total: { $sum: '$val' } } }]).toArray();
    }
    const savAggMs = performance.now() - tSavAgg;
    const tSqlAgg = performance.now();
    for (let i = 0; i < ITER_AGG; i++) {
      sqlSum.get();
    }
    const sqlAggMs = performance.now() - tSqlAgg;
    const savAggOps = +(ITER_AGG / (savAggMs / 1000)).toFixed(0);
    const sqlAggOps = +(ITER_AGG / (sqlAggMs / 1000)).toFixed(0);
    comparativeResults.push({
      label: `aggregate $group sum (${SCALE.toLocaleString()} docs)`,
      iterations: ITER_AGG,
      savannahOpsPerSec: savAggOps,
      sqliteOpsPerSec: sqlAggOps,
      ratio: +(savAggOps / sqlAggOps).toFixed(2),
    });
    process.stdout.write(
      `  · aggregate SUM over ${SCALE} docs        sav ${savAggOps.toLocaleString()}/s  sqlite ${sqlAggOps.toLocaleString()}/s\n`
    );

    sqliteBulk.close();
  }
}

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
  comparativePerf: comparativeResults,
  persistence: persistResults,
};

writeFileSync(OUT_FILE, JSON.stringify(report, null, 2));
console.log(`\nReport written: ${OUT_FILE}`);

if (mongoClient) await mongoClient.close();
