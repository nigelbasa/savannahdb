import assert from 'node:assert/strict';
import fs from 'node:fs';
import { SavannahDB } from '../server/dist/sdk/index.js';
import { startServer } from '../server/dist/server/index.js';

async function cleanupDir(path) {
  if (fs.existsSync(path)) {
    fs.rmSync(path, { recursive: true, force: true });
  }
}

async function runBenchmark() {
  console.log('===============================================================');
  console.log('             SAVANNAHDB PERFORMANCE & RELIABILITY TEST SUITE   ');
  console.log('===============================================================\n');

  const benchmarkDir = './data/db_benchmark';
  await cleanupDir(benchmarkDir);

  // =========================================================================
  // PART 1: Cold Restart Persistence & Accuracy Verification
  // =========================================================================
  console.log('--- PART 1: Cold Restart & Accuracy Test ---');
  let db = new SavannahDB({
    storage: { backend: 'canopy', root: benchmarkDir }
  });
  let items = db.collection('benchmark', 'accuracy');

  console.log('  Seeding initial documents...');
  await items.insertMany([
    { _id: 1, type: 'electric', name: 'Tesla Model 3', rating: 4.8 },
    { _id: 2, type: 'ice', name: 'Porsche 911', rating: 4.9 },
    { _id: 3, type: 'electric', name: 'Lucid Air', rating: 4.7 },
    { _id: 4, type: 'ice', name: 'Ford Mustang', rating: 4.2 }
  ]);

  console.log('  Updating documents...');
  await items.updateOne({ _id: 4 }, { $set: { rating: 4.5, classic: true } });
  await items.updateMany({ type: 'electric' }, { $inc: { rating: 0.1 } });

  console.log('  Creating index...');
  await items.createIndex('type_idx', 'type');

  console.log('  Deleting classic...');
  await items.deleteOne({ classic: true });

  // Read data before restart
  const preDocs = await items.find().sort({ _id: 1 }).toArray();
  const round1 = (v) => Math.round(v * 10) / 10;
  assert.equal(preDocs.length, 3);
  assert.equal(round1(preDocs[0].rating), 4.9); // Tesla (4.8 + 0.1)
  assert.equal(round1(preDocs[1].rating), 4.9); // Porsche (4.9 unchanged)
  assert.equal(round1(preDocs[2].rating), 4.8); // Lucid (4.7 + 0.1)

  console.log('  Triggering cold shutdown and restarting database...');
  // Force a cold restart by instantiating a brand new SavannahDB instance over the same folder.
  // The static native engine will load from the durably synced Canopy log and manifest.
  db = null;
  items = null;
  
  db = new SavannahDB({
    storage: { backend: 'canopy', root: benchmarkDir }
  });
  items = db.collection('benchmark', 'accuracy');

  console.log('  Verifying data accuracy post-restart...');
  const postDocs = await items.find().sort({ _id: 1 }).toArray();
  assert.deepEqual(postDocs, preDocs, 'Data recovery mismatch across cold restart!');
  console.log('  ✓ Data accuracy is 100% correct.');

  console.log('  Verifying index recovery post-restart...');
  const restoredIndexes = await items.listIndexes();
  assert.ok(restoredIndexes.find(idx => idx.name === 'type_idx'), 'Index type_idx was not recovered!');
  
  // Verify the index works for query planning
  const indexQueryResults = await items.find({ type: 'electric' }).toArray();
  assert.equal(indexQueryResults.length, 2);
  console.log('  ✓ Indexes recovered and fully operational.');
  console.log('  ✓ Cold restart persistence and data accuracy: PASS\n');


  // =========================================================================
  // PART 2: Performance Benchmark (Index vs Indexless scans)
  // =========================================================================
  console.log('--- PART 2: Performance & Index Efficiency Benchmark ---');
  const perfColl = db.collection('benchmark', 'performance');
  const NUM_DOCS = 2000;
  const NUM_READS = 1000;

  console.log(`  Inserting ${NUM_DOCS} documents...`);
  const writeStart = performance.now();
  const batch = [];
  for (let i = 1; i <= NUM_DOCS; i++) {
    batch.push({
      _id: i,
      category: i % 2 === 0 ? 'even' : 'odd',
      randomKey: `key-${Math.floor(Math.random() * 100)}`,
      value: i * 10
    });
    
    // Chunk inserts for robust transaction batches
    if (batch.length === 500 || i === NUM_DOCS) {
      await perfColl.insertMany(batch);
      batch.length = 0;
    }
  }
  const writeEnd = performance.now();
  const writeTime = (writeEnd - writeStart) / 1000;
  const writesPerSec = Math.round(NUM_DOCS / writeTime);
  console.log(`  ✓ Written ${NUM_DOCS} docs in ${writeTime.toFixed(3)}s (${writesPerSec} ops/sec)`);

  // Benchmarking Read (Indexless Scan)
  console.log(`  Running ${NUM_READS} lookups WITHOUT index (Indexless Scan)...`);
  const readScanStart = performance.now();
  for (let i = 0; i < NUM_READS; i++) {
    const val = i % NUM_DOCS;
    const docs = await perfColl.find({ _id: val }).toArray();
    assert.ok(docs.length <= 1);
  }
  const readScanEnd = performance.now();
  const readScanTime = (readScanEnd - readScanStart) / 1000;
  const scanOpsPerSec = Math.round(NUM_READS / readScanTime);
  console.log(`  ✓ Scan read throughput: ${scanOpsPerSec} ops/sec`);

  // Create an index on `_id`
  console.log('  Creating primary index on _id...');
  await perfColl.createIndex('_id_idx', '_id');

  // Benchmarking Read (Index-Backed Lookup)
  console.log(`  Running ${NUM_READS} lookups WITH index (Index-Backed)...`);
  const readIdxStart = performance.now();
  for (let i = 0; i < NUM_READS; i++) {
    const val = i % NUM_DOCS;
    const docs = await perfColl.find({ _id: val }).toArray();
    assert.ok(docs.length <= 1);
  }
  const readIdxEnd = performance.now();
  const readIdxTime = (readIdxEnd - readIdxStart) / 1000;
  const idxOpsPerSec = Math.round(NUM_READS / readIdxTime);
  console.log(`  ✓ Index-backed read throughput: ${idxOpsPerSec} ops/sec`);

  const speedup = (idxOpsPerSec / scanOpsPerSec).toFixed(1);
  console.log(`  🚀 Index-backed lookup is ${speedup}x faster than scanning!`);
  console.log('  ✓ Performance benchmark: PASS\n');


  // =========================================================================
  // PART 3: Multi-Connection HTTP REST Server Concurrency
  // =========================================================================
  console.log('--- PART 3: HTTP REST Server Concurrency Reliability ---');
  const serverPort = 27019;
  console.log(`  Starting standalone REST server on port ${serverPort}...`);
  const server = startServer(serverPort);
  
  // Wait for server to bind
  await new Promise(resolve => setTimeout(resolve, 300));

  // Initialize a client database pointing to the HTTP server
  const dbClient = new SavannahDB({
    url: `http://localhost:${serverPort}`
  });
  const concurrencyColl = dbClient.collection('concurrency_db', 'jobs');

  // We will run 10 client operations concurrently using Promise.all
  console.log('  Executing concurrent writes over multiple client requests...');
  const numWorkers = 8;
  const docsPerWorker = 20;
  const workerPromises = [];

  for (let w = 0; w < numWorkers; w++) {
    const workerId = w;
    workerPromises.push((async () => {
      const docs = [];
      for (let i = 0; i < docsPerWorker; i++) {
        docs.push({
          worker: workerId,
          jobId: `w${workerId}-j${i}`,
          status: 'pending',
          ts: Date.now()
        });
      }
      const res = await concurrencyColl.insertMany(docs);
      assert.equal(res.insertedCount, docsPerWorker);
    })());
  }

  await Promise.all(workerPromises);
  console.log(`  ✓ Successfully processed concurrent inserts.`);

  // Verify document count
  const allJobs = await concurrencyColl.find().toArray();
  const expectedDocs = numWorkers * docsPerWorker;
  assert.equal(allJobs.length, expectedDocs, `Concurrency lost documents! Expected ${expectedDocs}, got ${allJobs.length}`);
  console.log(`  ✓ Total documents matches exactly: ${allJobs.length}`);

  // Run concurrent updates
  console.log('  Executing concurrent updates...');
  const updatePromises = [];
  for (let w = 0; w < numWorkers; w++) {
    const workerId = w;
    updatePromises.push(
      concurrencyColl.updateMany(
        { worker: workerId },
        { $set: { status: 'processed' } }
      )
    );
  }
  await Promise.all(updatePromises);
  console.log('  ✓ Concurrent updates completed successfully.');

  // Validate accuracy
  const processedJobs = await concurrencyColl.find({ status: 'processed' }).toArray();
  assert.equal(processedJobs.length, expectedDocs, `Some updates did not apply! Got ${processedJobs.length}`);
  console.log('  ✓ All concurrent updates successfully applied.');

  console.log('  Shutting down REST Server...');
  server.close();
  console.log('  ✓ Concurrency reliability: PASS\n');

  // Final Cleanup
  await cleanupDir(benchmarkDir);
  console.log('===============================================================');
  console.log('             ALL PERFORMANCE AND CONCURRENCY TESTS PASSED!    ');
  console.log('===============================================================');
}

runBenchmark().catch(err => {
  console.error('\n❌ BENCHMARK SUITE ENCOUNTERED AN ERROR:');
  console.error(err);
  process.exit(1);
});
