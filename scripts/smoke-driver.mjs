// Connect with the real mongodb@6 driver and probe the handshake floor.
// This smoke is SavannahDB-focused, so it expects the emulated hello surface.
import { MongoClient } from 'mongodb';
import assert from 'node:assert/strict';
import { runAggregationDriverCases } from './smoke-driver/aggregation-cases.mjs';

const targetUri =
  process.env.SAVANNAH_URI ??
  'mongodb://127.0.0.1:27017/?serverSelectionTimeoutMS=3000&directConnection=true';
const dbName =
  process.env.SMOKE_DRIVER_DB ??
  `smoke_${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 8)}`;
const client = new MongoClient(targetUri);

try {
  await client.connect();
  console.log('connected');
  console.log('target uri:', targetUri);
  console.log('db name:', dbName);

  const admin = client.db('admin');
  const hello = await admin.command({ hello: 1 });
  console.log('hello response keys:', Object.keys(hello).join(', '));
  console.log('maxWireVersion:', hello.maxWireVersion);
  console.log('isWritablePrimary:', hello.isWritablePrimary);
  assert.equal(hello.maxWireVersion, 17);
  assert.equal(hello.isWritablePrimary, true);

  const zoo = client.db(dbName);
  const animals = zoo.collection('animals');

  await safe('ping', () => admin.command({ ping: 1 }));
  await safe('buildInfo', () => admin.command({ buildInfo: 1 }));
  await safe('getLog startupWarnings', () => admin.command({ getLog: 'startupWarnings' }));
  const commands = await safe('listCommands', () => admin.command({ listCommands: 1 }));
  assert.ok(commands?.commands?.find);
  assert.ok(commands?.commands?.aggregate);
  assert.ok(commands?.commands?.distinct);

  const ins = await safe('insertOne', () => animals.insertOne({ name: 'giraffe' }));
  assert.ok(ins?.insertedId);
  console.log('  insertedId:', ins.insertedId?.toString());

  const insertedMany = await safe('insertMany', () =>
    animals.insertMany([{ name: 'lion' }, { name: 'elephant' }]),
  );
  assert.equal(insertedMany.insertedCount, 2);

  const found = await safe('find toArray', () => animals.find({}).toArray());
  assert.equal(found.length, 3);
  console.log('  all docs:', found.length);

  const lions = await safe('find {name:lion}', () =>
    animals.find({ name: 'lion' }).toArray(),
  );
  assert.equal(lions.length, 1);
  assert.equal(lions[0].name, 'lion');
  console.log('  lion match:', JSON.stringify(lions));

  const one = await safe('findOne by _id', () =>
    animals.findOne({ _id: ins.insertedId }),
  );
  assert.ok(one);
  assert.equal(one.name, 'giraffe');
  console.log('  by-id:', one.name);

  const none = await safe('find no-match', () =>
    animals.find({ name: 'penguin' }).toArray(),
  );
  assert.equal(none.length, 0);
  console.log('  no-match length:', none.length);

  const big = zoo.collection('paginate');
  await safe('insertMany 25', () =>
    big.insertMany(
      Array.from({ length: 25 }, (_, i) => ({ n: i + 1, label: `doc-${i + 1}` })),
    ),
  );
  const all = await safe('find batchSize=10', () =>
    big.find({}, { batchSize: 10 }).toArray(),
  );
  assert.equal(all.length, 25);
  console.log('  paginated total:', all.length);

  const partial = await safe('cursor.next then close', async () => {
    const cur = big.find({}, { batchSize: 5 });
    const first = await cur.next();
    await cur.close();
    return first;
  });
  assert.ok(partial);
  assert.equal(partial.label, 'doc-1');
  console.log('  partial first:', partial.label);

  const mut = zoo.collection('mut');
  await safe('insertMany 5', () =>
    mut.insertMany(Array.from({ length: 5 }, (_, i) => ({ n: i + 1, hits: 0 }))),
  );

  const u1 = await safe('updateOne $set', () =>
    mut.updateOne({ n: 1 }, { $set: { name: 'first' } }),
  );
  assert.equal(u1.matchedCount, 1);
  assert.equal(u1.modifiedCount, 1);
  console.log('  matched/modified:', u1.matchedCount, u1.modifiedCount);

  const u2 = await safe('updateMany $inc', () =>
    mut.updateMany({}, { $inc: { hits: 1 } }),
  );
  assert.equal(u2.matchedCount, 5);
  assert.equal(u2.modifiedCount, 5);
  console.log('  multi matched/modified:', u2.matchedCount, u2.modifiedCount);

  const u3 = await safe('updateOne upsert', () =>
    mut.updateOne(
      { n: 999 },
      { $set: { label: 'inserted-by-upsert' } },
      { upsert: true },
    ),
  );
  assert.ok(u3.upsertedId);
  console.log('  upsertedId:', u3.upsertedId?.toString());

  const u4 = await safe('replaceOne', () =>
    mut.replaceOne({ n: 1 }, { n: 1, replaced: true }),
  );
  assert.equal(u4.matchedCount, 1);
  assert.equal(u4.modifiedCount, 1);
  console.log('  replaced:', u4.matchedCount, u4.modifiedCount);

  const d1 = await safe('deleteOne', () => mut.deleteOne({ n: 2 }));
  assert.equal(d1.deletedCount, 1);
  console.log('  deleted one:', d1.deletedCount);

  const d2 = await safe('deleteMany', () => mut.deleteMany({ hits: 1 }));
  assert.equal(d2.deletedCount, 3);
  console.log('  deleted many:', d2.deletedCount);

  const remaining = await safe('count via find', async () =>
    (await mut.find({}).toArray()).length,
  );
  assert.equal(remaining, 2);
  console.log('  remaining:', remaining);

  const nested = zoo.collection('nested_wire');
  await safe('insertMany nested', () =>
    nested.insertMany([
      { city: { name: 'NYC' }, pop: 8000 },
      { city: { name: 'SF' }, pop: 900 },
      { city: { name: 'LA' }, pop: 4000 },
    ]),
  );
  const nyc = await safe('find {city.name:NYC}', () =>
    nested.find({ 'city.name': 'NYC' }).toArray(),
  );
  assert.equal(nyc.length, 1);
  assert.equal(nyc[0].city.name, 'NYC');
  console.log('  nyc count:', nyc.length);

  const sorted = await safe('find sort+limit+skip', () =>
    nested.find({}).sort({ pop: -1 }).skip(1).limit(1).toArray(),
  );
  assert.deepEqual(sorted.map((doc) => doc.city.name), ['LA']);
  console.log('  sort window:', sorted.map((doc) => doc.city.name));

  const mixedSort = zoo.collection('mixed_sort');
  await safe('insertMany mixed sort', () =>
    mixedSort.insertMany([
      { label: 'missing' },
      { label: 'null', v: null },
      { label: 'number', v: 5 },
      { label: 'string', v: 'zzz' },
      { label: 'bool', v: true },
    ]),
  );
  const mixedAsc = await safe('find mixed-type sort asc', () =>
    mixedSort.find({}).sort({ v: 1 }).project({ _id: 0, label: 1 }).toArray(),
  );
  assert.deepEqual(
    mixedAsc.map((doc) => doc.label),
    ['missing', 'null', 'number', 'string', 'bool'],
  );
  const mixedDesc = await safe('find mixed-type sort desc', () =>
    mixedSort.find({}).sort({ v: -1 }).project({ _id: 0, label: 1 }).toArray(),
  );
  assert.deepEqual(
    mixedDesc.map((doc) => doc.label),
    ['bool', 'string', 'number', 'missing', 'null'],
  );
  console.log('  mixed sort asc:', mixedAsc.map((doc) => doc.label));
  console.log('  mixed sort desc:', mixedDesc.map((doc) => doc.label));

  const indexedSparseSort = zoo.collection('indexed_sparse_sort');
  await safe('insertMany indexed sparse sort', () =>
    indexedSparseSort.insertMany([
      { label: 'missing-a' },
      { label: 'null-a', v: null },
      { label: 'missing-b' },
      { label: 'null-b', v: null },
      { label: 'number', v: 5 },
      { label: 'string', v: 'zzz' },
      { label: 'bool', v: true },
    ]),
  );
  await safe('createIndex indexed sparse sort', () =>
    indexedSparseSort.createIndex({ v: 1 }, { name: 'v_1' }),
  );
  const indexedSparseAsc = await safe('find indexed sparse sort asc', () =>
    indexedSparseSort.find({}).sort({ v: 1 }).project({ _id: 0, label: 1 }).toArray(),
  );
  assert.deepEqual(
    indexedSparseAsc.map((doc) => doc.label),
    ['missing-a', 'null-a', 'missing-b', 'null-b', 'number', 'string', 'bool'],
  );
  const indexedSparseDesc = await safe('find indexed sparse sort desc', () =>
    indexedSparseSort.find({}).sort({ v: -1 }).project({ _id: 0, label: 1 }).toArray(),
  );
  assert.deepEqual(
    indexedSparseDesc.map((doc) => doc.label),
    ['bool', 'string', 'number', 'missing-a', 'null-a', 'missing-b', 'null-b'],
  );
  console.log('  indexed sparse asc:', indexedSparseAsc.map((doc) => doc.label));
  console.log('  indexed sparse desc:', indexedSparseDesc.map((doc) => doc.label));

  const projected = await safe('find projection', () =>
    nested.find({}, { projection: { pop: 1, _id: 0 } }).toArray(),
  );
  assert.equal(projected.length, 3);
  for (const doc of projected) {
    assert.deepEqual(Object.keys(doc).sort(), ['pop']);
  }
  console.log('  projected keys:', Object.keys(projected[0]).sort());

  const distinctKinds = await safe('distinct basic values', async () => {
    const distinctColl = zoo.collection('distinct_basic');
    await distinctColl.insertMany([
      { kind: 'cat' },
      { kind: 'cat' },
      { kind: 'dog' },
    ]);
    return distinctColl.distinct('kind');
  });
  assert.deepEqual(distinctKinds.slice().sort(), ['cat', 'dog']);
  console.log('  distinct values:', distinctKinds.slice().sort());

  await runAggregationDriverCases({ zoo, safe, assert });
} catch (e) {
  console.error('DRIVER ERROR:', e.message);
  process.exitCode = 1;
} finally {
  await client.close();
}

async function safe(label, fn) {
  try {
    const result = await fn();
    console.log(`${label} OK`);
    return result;
  } catch (e) {
    console.log(`${label} FAIL: ${e.message}`);
    throw e;
  }
}
