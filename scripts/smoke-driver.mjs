// Connect with the real mongodb@6 driver and probe the handshake floor.
// Runs commands the driver needs for a basic CRUD session so we can see
// which unknown commands the server must stub for Slice A.
import { MongoClient } from 'mongodb';

const client = new MongoClient(
  'mongodb://127.0.0.1:27017/?serverSelectionTimeoutMS=3000&directConnection=true',
);
try {
  await client.connect();
  console.log('connected');
  const admin = client.db('admin');
  const hello = await admin.command({ hello: 1 });
  console.log('hello response keys:', Object.keys(hello).join(', '));
  console.log('maxWireVersion:', hello.maxWireVersion);
  console.log('isWritablePrimary:', hello.isWritablePrimary);

  const zoo = client.db('zoo');
  const animals = zoo.collection('animals');

  // Drive through the commands Slice A will need.
  await safe('ping', () => admin.command({ ping: 1 }));
  await safe('buildInfo', () => admin.command({ buildInfo: 1 }));
  await safe('getLog startupWarnings', () => admin.command({ getLog: 'startupWarnings' }));
  const ins = await safe('insertOne', () => animals.insertOne({ name: 'giraffe' }));
  if (ins) console.log('  insertedId:', ins.insertedId?.toString());
  await safe('insertMany', () =>
    animals.insertMany([{ name: 'lion' }, { name: 'elephant' }]),
  );
  const found = await safe('find toArray', () => animals.find({}).toArray());
  if (found) console.log('  all docs:', found.length);

  // C1b: filter through the wire — string equality on a top-level field.
  const lions = await safe('find {name:lion}', () =>
    animals.find({ name: 'lion' }).toArray(),
  );
  if (lions) console.log('  lion match:', JSON.stringify(lions));

  // findOne by _id — the canonical driver path (ObjectId equality).
  if (ins?.insertedId) {
    const one = await safe('findOne by _id', () =>
      animals.findOne({ _id: ins.insertedId }),
    );
    if (one) console.log('  by-id:', one.name);
  }

  // No-match returns empty cursor.
  const none = await safe('find no-match', () =>
    animals.find({ name: 'penguin' }).toArray(),
  );
  if (none) console.log('  no-match length:', none.length);

  // Slice D: pagination via getMore. Insert 25 docs, set batchSize=10 → the
  // driver must do 1 find + 2 getMore round-trips + an implicit killCursors
  // (or natural close on cursorId=0). All transparent through .toArray().
  const big = zoo.collection('paginate');
  await safe('insertMany 25', () =>
    big.insertMany(
      Array.from({ length: 25 }, (_, i) => ({ n: i + 1, label: `doc-${i + 1}` })),
    ),
  );
  const all = await safe('find batchSize=10', () =>
    big.find({}, { batchSize: 10 }).toArray(),
  );
  if (all) console.log('  paginated total:', all.length);

  // Early-exit cursor → driver issues killCursors before exhaustion.
  const partial = await safe('cursor.next then close', async () => {
    const cur = big.find({}, { batchSize: 5 });
    const first = await cur.next();
    await cur.close();
    return first;
  });
  if (partial) console.log('  partial first:', partial.label);

  // Slice E: update + delete through the wire.
  const mut = zoo.collection('mut');
  await safe('insertMany 5', () =>
    mut.insertMany(Array.from({ length: 5 }, (_, i) => ({ n: i + 1, hits: 0 }))),
  );
  // updateOne with $set
  const u1 = await safe('updateOne $set', () =>
    mut.updateOne({ n: 1 }, { $set: { name: 'first' } }),
  );
  if (u1) console.log('  matched/modified:', u1.matchedCount, u1.modifiedCount);

  // updateMany $inc
  const u2 = await safe('updateMany $inc', () =>
    mut.updateMany({}, { $inc: { hits: 1 } }),
  );
  if (u2) console.log('  multi matched/modified:', u2.matchedCount, u2.modifiedCount);

  // findOneAndUpdate-style upsert.
  const u3 = await safe('updateOne upsert', () =>
    mut.updateOne(
      { n: 999 },
      { $set: { label: 'inserted-by-upsert' } },
      { upsert: true },
    ),
  );
  if (u3) console.log('  upsertedId:', u3.upsertedId?.toString());

  // replaceOne preserves _id automatically.
  const u4 = await safe('replaceOne', () =>
    mut.replaceOne({ n: 1 }, { n: 1, replaced: true }),
  );
  if (u4) console.log('  replaced:', u4.matchedCount, u4.modifiedCount);

  // deleteOne
  const d1 = await safe('deleteOne', () => mut.deleteOne({ n: 2 }));
  if (d1) console.log('  deleted one:', d1.deletedCount);

  // deleteMany
  const d2 = await safe('deleteMany', () => mut.deleteMany({ hits: 1 }));
  if (d2) console.log('  deleted many:', d2.deletedCount);

  // Final tally via find — countDocuments uses aggregate (Phase 0.4).
  const remaining = await safe('count via find', async () =>
    (await mut.find({}).toArray()).length,
  );
  if (typeof remaining === 'number') console.log('  remaining:', remaining);

  // Slice F1: nested-path filter through the wire.
  const nested = zoo.collection('nested_wire');
  await safe('insertMany nested', () =>
    nested.insertMany([
      { city: { name: 'NYC' }, pop: 8000 },
      { city: { name: 'SF' },  pop: 900 },
      { city: { name: 'LA' },  pop: 4000 },
    ]),
  );
  const nyc = await safe('find {city.name:NYC}', () =>
    nested.find({ 'city.name': 'NYC' }).toArray(),
  );
  if (nyc) console.log('  nyc count:', nyc.length);

  // Slice F2: sort/limit/skip through the wire.
  const sorted = await safe('find sort+limit+skip', () =>
    nested.find({}).sort({ pop: -1 }).skip(1).limit(1).toArray(),
  );
  if (sorted) console.log('  sort window:', sorted.map((d) => d.city.name));

  // Slice F3: projection through the wire (top-level only — dotted is Phase 0.4).
  const projected = await safe('find projection', () =>
    nested.find({}, { projection: { pop: 1, _id: 0 } }).toArray(),
  );
  if (projected) console.log('  projected keys:', Object.keys(projected[0]).sort());
} catch (e) {
  console.error('DRIVER ERROR:', e.message);
  process.exitCode = 1;
} finally {
  await client.close();
}

async function safe(label, fn) {
  try {
    const r = await fn();
    console.log(`${label} OK`);
    return r;
  } catch (e) {
    console.log(`${label} FAIL: ${e.message}`);
  }
}
