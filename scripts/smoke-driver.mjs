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

  // Phase 0.4: aggregation pipeline.
  const agg = zoo.collection('agg');
  await safe('insertMany aggregate source', () =>
    agg.insertMany([
      { kind: 'cat', score: 2 },
      { kind: 'cat', score: 5 },
      { kind: 'dog', score: 3 },
    ]),
  );
  const counted = await safe('countDocuments', () => agg.countDocuments({ kind: 'cat' }));
  if (typeof counted === 'number') console.log('  countDocuments:', counted);

  const grouped = await safe('aggregate $match+$group', () =>
    agg.aggregate([
      { $match: { kind: 'cat' } },
      { $group: { _id: '$kind', total: { $sum: 1 } } },
    ]).toArray(),
  );
  if (grouped) console.log('  grouped:', JSON.stringify(grouped));

  const groupedStats = await safe('aggregate group accumulators', () =>
    agg.aggregate([
      {
        $sort: { score: 1 },
      },
      {
        $group: {
          _id: '$kind',
          minScore: { $min: '$score' },
          maxScore: { $max: '$score' },
          avgScore: { $avg: '$score' },
          lastScore: { $last: '$score' },
        },
      },
      { $sort: { _id: 1 } },
    ]).toArray(),
  );
  if (groupedStats) console.log('  grouped stats:', JSON.stringify(groupedStats));

  const sortedWindow = await safe('aggregate $sort+$skip+$limit', () =>
    agg.aggregate([
      { $sort: { score: -1 } },
      { $skip: 1 },
      { $limit: 1 },
    ]).toArray(),
  );
  if (sortedWindow) console.log('  sorted window:', JSON.stringify(sortedWindow));

  const projectedAgg = await safe('aggregate $project', () =>
    agg.aggregate([
      {
        $project: {
          _id: 0,
          kind: 1,
          renamedScore: '$score',
          tag: 'seen',
        },
      },
    ]).toArray(),
  );
  if (projectedAgg) console.log('  projected aggregate:', JSON.stringify(projectedAgg));

  const expressionDepth = await safe('aggregate expression depth', () =>
    agg.aggregate([
      {
        $project: {
          _id: 0,
          label: { $concat: ['$kind', ':', { $toString: '$score' }] },
          fallback: { $ifNull: ['$missing', 'fallback'] },
        },
      },
      { $sort: { label: 1 } },
    ]).toArray(),
  );
  if (expressionDepth) console.log('  expression depth:', JSON.stringify(expressionDepth));

  const arithmetic = await safe('aggregate arithmetic+comparison', () =>
    agg.aggregate([
      { $sort: { score: 1 } },
      {
        $project: {
          _id: 0,
          kind: 1,
          score: 1,
          doubled: { $multiply: ['$score', 2] },
          plusOne: { $add: ['$score', 1] },
          half: { $divide: ['$score', 2] },
          remainder: { $mod: ['$score', 2] },
          delta: { $subtract: ['$score', 3] },
          absDelta: { $abs: { $subtract: ['$score', 3] } },
          isCat: { $eq: ['$kind', 'cat'] },
          highScore: { $gt: ['$score', 3] },
          rounded: { $round: [{ $divide: ['$score', 3] }, 2] },
        },
      },
    ]).toArray(),
  );
  if (arithmetic) console.log('  arithmetic:', JSON.stringify(arithmetic));

  const conditional = await safe('aggregate conditional+boolean', () =>
    agg.aggregate([
      { $sort: { score: 1 } },
      {
        $project: {
          _id: 0,
          kind: 1,
          score: 1,
          tier: {
            $cond: {
              if: { $gte: ['$score', 5] },
              then: 'high',
              else: 'low',
            },
          },
          bucket: {
            $switch: {
              branches: [
                { case: { $lt: ['$score', 3] }, then: 'small' },
                { case: { $lt: ['$score', 5] }, then: 'medium' },
              ],
              default: 'large',
            },
          },
          isCatHigh: { $and: [{ $eq: ['$kind', 'cat'] }, { $gte: ['$score', 5] }] },
          catOrLow: { $or: [{ $eq: ['$kind', 'cat'] }, { $lt: ['$score', 3] }] },
          notCat: { $not: { $eq: ['$kind', 'cat'] } },
        },
      },
    ]).toArray(),
  );
  if (conditional) console.log('  conditional:', JSON.stringify(conditional));

  const stringOps = await safe('aggregate string ops', () =>
    agg.aggregate([
      { $sort: { score: 1 } },
      { $limit: 1 },
      {
        $project: {
          _id: 0,
          upper: { $toUpper: '$kind' },
          lower: { $toLower: 'CAT' },
          trimmed: { $trim: { input: '  hello  ' } },
          trimChars: { $trim: { input: '--cat--', chars: '-' } },
          first3: { $substr: ['$kind', 0, 3] },
          parts: { $split: ['a,b,c', ','] },
          len: { $strLenCP: '$kind' },
        },
      },
    ]).toArray(),
  );
  if (stringOps) console.log('  string ops:', JSON.stringify(stringOps));

  const addFieldsAgg = await safe('aggregate $set+$unset+$count', () =>
    agg.aggregate([
      { $set: { renamedKind: '$kind', seen: true } },
      { $unset: ['kind', 'score'] },
      { $match: { renamedKind: 'cat' } },
      { $count: 'totalCats' },
    ]).toArray(),
  );
  if (addFieldsAgg) console.log('  set/unset/count:', JSON.stringify(addFieldsAgg));

  const arrayExpressions = await safe('aggregate array expressions', () =>
    agg.aggregate([
      {
        $addFields: {
          bundle: ['$kind', '$score'],
          maybeScore: { $ifNull: ['$score', 0] },
        },
      },
      {
        $project: {
          _id: 0,
          firstItem: { $arrayElemAt: ['$bundle', 0] },
          bundleSize: { $size: '$bundle' },
          maybeScore: 1,
          literalDollar: { $literal: '$score' },
        },
      },
      { $sort: { firstItem: 1, maybeScore: 1 } },
    ]).toArray(),
  );
  if (arrayExpressions) console.log('  array expressions:', JSON.stringify(arrayExpressions));

  const sortByCountAgg = await safe('aggregate $sortByCount', () =>
    agg.aggregate([
      { $sortByCount: '$kind' },
    ]).toArray(),
  );
  if (sortByCountAgg) console.log('  sortByCount:', JSON.stringify(sortByCountAgg));

  const aggregateCursor = await safe('aggregate cursor getMore', async () => {
    const cursor = agg.aggregate(
      [{ $sort: { score: 1 } }],
      { cursor: { batchSize: 1 } },
    );
    const docs = await cursor.toArray();
    return docs;
  });
  if (aggregateCursor) console.log('  aggregate cursor total:', aggregateCursor.length);

  const unwindSource = zoo.collection('unwind_cases');
  await safe('insertMany unwind cases', () =>
    unwindSource.insertMany([
      { name: 'alpha', tags: ['x', 'y'] },
      { name: 'beta', tags: [] },
      { name: 'gamma' },
    ]),
  );
  const unwindOptions = await safe('aggregate $unwind options', () =>
    unwindSource.aggregate([
      {
        $unwind: {
          path: '$tags',
          preserveNullAndEmptyArrays: true,
          includeArrayIndex: 'tagIndex',
        },
      },
      { $project: { _id: 0, name: 1, tags: 1, tagIndex: 1 } },
      { $sort: { name: 1, tagIndex: 1 } },
    ]).toArray(),
  );
  if (unwindOptions) console.log('  unwind options:', JSON.stringify(unwindOptions));

  const owners = zoo.collection('owners');
  const pets = zoo.collection('pets');
  const ownerIds = (await safe('insertMany owners', () =>
    owners.insertMany([{ name: 'alice' }, { name: 'bob' }]),
  ))?.insertedIds;
  if (ownerIds) {
    await safe('insertMany pets', () =>
      pets.insertMany([
        { ownerId: ownerIds[0], name: 'milo' },
        { ownerId: ownerIds[0], name: 'otis' },
        { ownerId: ownerIds[1], name: 'luna' },
      ]),
    );
    const joined = await safe('aggregate $lookup+$unwind', () =>
      owners.aggregate([
        { $lookup: { from: 'pets', localField: '_id', foreignField: 'ownerId', as: 'pets' } },
        { $unwind: '$pets' },
        { $match: { 'pets.name': 'milo' } },
        { $project: { _id: 0, owner: '$name', pet: '$pets.name' } },
      ]).toArray(),
    );
    if (joined) console.log('  lookup+unwind:', JSON.stringify(joined));

    const replaced = await safe('aggregate $replaceRoot/$replaceWith', () =>
      owners.aggregate([
        { $lookup: { from: 'pets', localField: '_id', foreignField: 'ownerId', as: 'pets' } },
        { $unwind: '$pets' },
        { $replaceRoot: { newRoot: '$pets' } },
        { $match: { name: 'milo' } },
        {
          $replaceWith: {
            petName: '$name',
            ownerRef: '$ownerId',
            summary: { $concat: ['$name', ':', { $toString: '$ownerId' }] },
          },
        },
      ]).toArray(),
    );
    if (replaced) console.log('  replaceRoot/replaceWith:', JSON.stringify(replaced));
  }
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
