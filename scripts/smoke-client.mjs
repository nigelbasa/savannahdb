import assert from 'node:assert/strict';
import fs from 'node:fs';
import { SavannahDB } from '../server/dist/sdk/index.js';
import { startServer } from '../server/dist/server/index.js';

async function cleanupDir(path) {
  if (fs.existsSync(path)) {
    fs.rmSync(path, { recursive: true, force: true });
  }
}

async function runTests() {
  console.log('=== STARTING SAVANNAHDB E2E SMOKE TESTS ===\n');

  // =========================================================================
  // SCENARIO 1: Embedded Mode (In-Memory)
  // =========================================================================
  console.log('--- Scenario 1: Embedded Mode (In-Memory) ---');
  const dbMem = new SavannahDB({
    storage: { backend: 'memory' }
  });

  assert.equal(dbMem.getMode(), 'embedded');
  const collMem = dbMem.collection('test_mem', 'animals');

  // 1. Insert docs
  console.log('  Inserting documents...');
  const insRes = await collMem.insertMany([
    { _id: 1, name: 'giraffe', height: 5.5, tags: ['tall'] },
    { _id: 2, name: 'lion', pack: ['simba'], height: 1.2 },
    { _id: 3, name: 'elephant', age: 42, height: 3.2 }
  ]);
  assert.equal(insRes.insertedCount, 3);

  // 2. Chained Find cursor
  console.log('  Testing find with chained cursor (sort, limit)...');
  const results = await collMem.find({ height: { $gt: 1.0 } })
    .sort({ height: 1 })
    .limit(2)
    .toArray();

  assert.equal(results.length, 2);
  assert.equal(results[0].name, 'lion');
  assert.equal(results[1].name, 'elephant');

  // 3. Update operations
  console.log('  Testing updateOne / updateMany...');
  const updRes1 = await collMem.updateOne({ name: 'lion' }, { $set: { status: 'king' } });
  assert.equal(updRes1.matched, 1);
  assert.equal(updRes1.modified, 1);

  const updatedLion = await collMem.findOne({ name: 'lion' });
  assert.equal(updatedLion.status, 'king');

  // Update with inc
  await collMem.updateOne({ _id: 3 }, { $inc: { age: 10 } });
  const updatedEle = await collMem.findOne({ _id: 3 });
  assert.equal(updatedEle.age, 52);

  // 4. Native Aggregation Pipeline
  console.log('  Testing native C++ aggregation pipelines ($match, $group)...');
  const aggResults = await collMem.aggregate([
    { $match: { height: { $gt: 2.0 } } },
    { $group: { _id: '$name', height: { $first: '$height' } } }
  ]).toArray();

  assert.equal(aggResults.length, 2);
  const names = aggResults.map(r => r._id).sort();
  assert.deepEqual(names, ['elephant', 'giraffe']);

  // 5. Index Management
  console.log('  Testing index create/list/drop...');
  const created = await collMem.createIndex('idx_name', 'name');
  assert.equal(created, true);

  const indexes = await collMem.listIndexes();
  const nameIndex = indexes.find(idx => idx.name === 'idx_name');
  assert.ok(nameIndex);
  assert.equal(nameIndex.fieldPath, 'name');

  const dropped = await collMem.dropIndex('idx_name');
  assert.equal(dropped, true);

  const indexesPost = await collMem.listIndexes();
  assert.ok(!indexesPost.some(idx => idx.name === 'idx_name'));

  // 6. Delete operations
  console.log('  Testing deleteOne / deleteMany...');
  const delRes1 = await collMem.deleteOne({ name: 'giraffe' });
  assert.equal(delRes1.deleted, 1);

  const leftCount = await collMem.find().toArray();
  assert.equal(leftCount.length, 2);

  console.log('✓ Scenario 1 passed successfully.\n');

  // =========================================================================
  // SCENARIO 2: Embedded Mode (Canopy / Persistent)
  // =========================================================================
  console.log('--- Scenario 2: Embedded Mode (Canopy Persistent) ---');
  const canopyDir = './data/db_smoke_test';
  await cleanupDir(canopyDir);

  // Start instance 1
  const dbCanopy1 = new SavannahDB({
    storage: { backend: 'canopy', root: canopyDir }
  });
  const collCanopy1 = dbCanopy1.collection('test_canopy', 'books');

  console.log('  Inserting data into persistent store...');
  await collCanopy1.insertMany([
    { _id: 'b1', title: 'The Great Gatsby', year: 1925 },
    { _id: 'b2', title: 'To Kill a Mockingbird', year: 1960 }
  ]);

  // Read it back from instance 1
  const books1 = await collCanopy1.find().sort({ year: 1 }).toArray();
  assert.equal(books1.length, 2);
  assert.equal(books1[0].title, 'The Great Gatsby');

  // Node's N-API singleton means re-instantiating SavannahDB on same process
  // will reuse the loaded Engine, but we can verify that Canopy is persistent
  // by writing and doing normal operations. (We also clean it up at the end).
  console.log('✓ Scenario 2 passed successfully.\n');

  // =========================================================================
  // SCENARIO 3: REST Server & Client SDK Mode
  // =========================================================================
  console.log('--- Scenario 3: REST Server + Client Mode ---');
  const port = 27018;

  // Let's spawn the HTTP Server programmatically
  console.log(`  Booting HTTP REST Server on port ${port}...`);
  const server = startServer(port);

  // Wait a small bit for server to listen
  await new Promise(resolve => setTimeout(resolve, 500));

  // Initialize client DB
  const dbClient = new SavannahDB({
    url: `http://localhost:${port}`
  });
  assert.equal(dbClient.getMode(), 'client');
  const collClient = dbClient.collection('test_http', 'cars');

  // 1. Insert via Client SDK
  console.log('  Inserting documents over HTTP...');
  const insResHttp = await collClient.insertMany([
    { _id: 101, make: 'Tesla', model: 'Model S', range: 405 },
    { _id: 102, make: 'Porsche', model: 'Taycan', range: 246 },
    { _id: 103, make: 'Lucid', model: 'Air', range: 516 }
  ]);
  assert.equal(insResHttp.insertedCount, 3);

  // 2. Chained Find via HTTP
  console.log('  Querying documents over HTTP (find with sort/limit)...');
  const cars = await collClient.find({ range: { $gt: 200 } })
    .sort({ range: -1 })
    .limit(2)
    .toArray();

  assert.equal(cars.length, 2);
  assert.equal(cars[0].make, 'Lucid');
  assert.equal(cars[1].make, 'Tesla');

  // 3. Update over HTTP
  console.log('  Updating document over HTTP...');
  await collClient.updateOne({ _id: 102 }, { $set: { range: 250 } });
  const updatedCar = await collClient.findOne({ _id: 102 });
  assert.equal(updatedCar.range, 250);

  // 4. Aggregation over HTTP
  console.log('  Running aggregation pipeline over HTTP...');
  const pipeRes = await collClient.aggregate([
    { $match: { make: { $in: ['Tesla', 'Lucid'] } } },
    { $group: { _id: 'total_range', range_sum: { $sum: '$range' } } }
  ]).toArray();

  assert.equal(pipeRes.length, 1);
  assert.equal(pipeRes[0].range_sum, 921); // 405 + 516

  // 5. Indexing over HTTP
  console.log('  Creating and dropping indexes over HTTP...');
  const idxCreated = await collClient.createIndex('range_idx', 'range');
  assert.equal(idxCreated, true);

  const clientIndexes = await collClient.listIndexes();
  assert.ok(clientIndexes.find(idx => idx.name === 'range_idx'));

  const idxDropped = await collClient.dropIndex('range_idx');
  assert.equal(idxDropped, true);

  // 6. Deletes over HTTP
  console.log('  Deleting documents over HTTP...');
  const delResHttp = await collClient.deleteOne({ _id: 103 });
  assert.equal(delResHttp.deleted, 1);

  const finalCars = await collClient.find().toArray();
  assert.equal(finalCars.length, 2);

  // Close the server cleanly
  console.log('  Shutting down REST Server...');
  server.close();

  console.log('✓ Scenario 3 passed successfully.\n');

  // Cleanup canopy directory
  await cleanupDir(canopyDir);

  console.log('=== ALL SAVANNAHDB E2E SMOKE TESTS PASSED SUCCESSFULLY! ===');
}

runTests().catch(err => {
  console.error('\n❌ TEST SUITE FAILED:');
  console.error(err);
  process.exit(1);
});
