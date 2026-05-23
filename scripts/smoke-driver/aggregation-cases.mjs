export async function runAggregationDriverCases({ zoo, safe, assert }) {
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
  assert.equal(counted, 2);
  console.log('  countDocuments:', counted);

  const grouped = await safe('aggregate $match+$group', () =>
    agg.aggregate([
      { $match: { kind: 'cat' } },
      { $group: { _id: '$kind', total: { $sum: 1 } } },
    ]).toArray(),
  );
  assert.deepEqual(grouped, [{ _id: 'cat', total: 2 }]);
  console.log('  grouped:', JSON.stringify(grouped));

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
  assert.deepEqual(groupedStats, [
    { _id: 'cat', minScore: 2, maxScore: 5, avgScore: 3.5, lastScore: 5 },
    { _id: 'dog', minScore: 3, maxScore: 3, avgScore: 3, lastScore: 3 },
  ]);
  console.log('  grouped stats:', JSON.stringify(groupedStats));

  const sparseGrouped = await safe('aggregate sparse $group semantics', async () => {
    const sparse = zoo.collection('agg_sparse');
    await sparse.insertMany([
      { bucket: 'a', value: 1 },
      { value: 2 },
      { bucket: 'a' },
      {},
    ]);
    return sparse.aggregate([
      {
        $group: {
          _id: '$bucket',
          firstValue: { $first: '$value' },
          lastValue: { $last: '$value' },
          values: { $push: '$value' },
          count: { $sum: 1 },
        },
      },
    ]).toArray();
  });
  assert.equal(sparseGrouped.length, 2);
  const sparseGroupA = sparseGrouped.find((doc) => doc._id === 'a');
  const sparseGroupNull = sparseGrouped.find((doc) => doc._id === null);
  assert.deepEqual(sparseGroupA, {
    _id: 'a',
    firstValue: 1,
    lastValue: null,
    values: [1],
    count: 2,
  });
  assert.deepEqual(sparseGroupNull, {
    _id: null,
    firstValue: 2,
    lastValue: null,
    values: [2],
    count: 2,
  });
  console.log('  sparse group:', JSON.stringify(sparseGrouped));

  const sortedWindow = await safe('aggregate $sort+$skip+$limit', () =>
    agg.aggregate([
      { $sort: { score: -1 } },
      { $skip: 1 },
      { $limit: 1 },
    ]).toArray(),
  );
  assert.equal(sortedWindow.length, 1);
  assert.equal(sortedWindow[0].kind, 'dog');
  assert.equal(sortedWindow[0].score, 3);
  console.log('  sorted window:', JSON.stringify(sortedWindow));

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
  assert.deepEqual(
    projectedAgg.slice().sort((left, right) => left.renamedScore - right.renamedScore),
    [
      { kind: 'cat', renamedScore: 2, tag: 'seen' },
      { kind: 'dog', renamedScore: 3, tag: 'seen' },
      { kind: 'cat', renamedScore: 5, tag: 'seen' },
    ],
  );
  console.log('  projected aggregate:', JSON.stringify(projectedAgg));

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
  assert.deepEqual(expressionDepth, [
    { label: 'cat:2', fallback: 'fallback' },
    { label: 'cat:5', fallback: 'fallback' },
    { label: 'dog:3', fallback: 'fallback' },
  ]);
  console.log('  expression depth:', JSON.stringify(expressionDepth));

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
  assert.deepEqual(arithmetic, [
    {
      kind: 'cat',
      score: 2,
      doubled: 4,
      plusOne: 3,
      half: 1,
      remainder: 0,
      delta: -1,
      absDelta: 1,
      isCat: true,
      highScore: false,
      rounded: 0.67,
    },
    {
      kind: 'dog',
      score: 3,
      doubled: 6,
      plusOne: 4,
      half: 1.5,
      remainder: 1,
      delta: 0,
      absDelta: 0,
      isCat: false,
      highScore: false,
      rounded: 1,
    },
    {
      kind: 'cat',
      score: 5,
      doubled: 10,
      plusOne: 6,
      half: 2.5,
      remainder: 1,
      delta: 2,
      absDelta: 2,
      isCat: true,
      highScore: true,
      rounded: 1.67,
    },
  ]);
  console.log('  arithmetic:', JSON.stringify(arithmetic));

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
  assert.deepEqual(conditional, [
    {
      kind: 'cat',
      score: 2,
      tier: 'low',
      bucket: 'small',
      isCatHigh: false,
      catOrLow: true,
      notCat: false,
    },
    {
      kind: 'dog',
      score: 3,
      tier: 'low',
      bucket: 'medium',
      isCatHigh: false,
      catOrLow: false,
      notCat: true,
    },
    {
      kind: 'cat',
      score: 5,
      tier: 'high',
      bucket: 'large',
      isCatHigh: true,
      catOrLow: true,
      notCat: false,
    },
  ]);
  console.log('  conditional:', JSON.stringify(conditional));

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
  assert.deepEqual(stringOps, [
    {
      upper: 'CAT',
      lower: 'cat',
      trimmed: 'hello',
      trimChars: 'cat',
      first3: 'cat',
      parts: ['a', 'b', 'c'],
      len: 3,
    },
  ]);
  console.log('  string ops:', JSON.stringify(stringOps));

  const typeOps = await safe('aggregate type ops', () =>
    agg.aggregate([
      { $sort: { score: 1 } },
      { $limit: 1 },
      {
        $project: {
          _id: 0,
          scoreType: { $type: '$score' },
          kindType: { $type: '$kind' },
          missingType: { $type: '$nonexistent' },
          parsedInt: { $toInt: '42' },
          parsedDouble: { $toDouble: '3.14' },
          boolFromInt: { $toBool: '$score' },
          boolFromString: { $toBool: '' },
          isScoreNum: { $isNumber: '$score' },
          isKindNum: { $isNumber: '$kind' },
        },
      },
    ]).toArray(),
  );
  assert.deepEqual(typeOps, [
    {
      scoreType: 'int',
      kindType: 'string',
      missingType: 'missing',
      parsedInt: 42,
      parsedDouble: 3.14,
      boolFromInt: true,
      boolFromString: false,
      isScoreNum: true,
      isKindNum: false,
    },
  ]);
  console.log('  type ops:', JSON.stringify(typeOps));

  const arrayMath = await safe('aggregate array+math ops', () =>
    agg.aggregate([
      { $sort: { score: 1 } },
      { $limit: 1 },
      {
        $project: {
          _id: 0,
          summed: { $sum: [1, 2, 3, 4] },
          summedField: { $sum: '$score' },
          averaged: { $avg: [10, 20, 30] },
          minVal: { $min: [3, 1, 2] },
          maxVal: { $max: [3, 1, 2] },
          power: { $pow: [2, 8] },
          rooted: { $sqrt: 16 },
          logBase: { $log: [100, 10] },
          merged: { $concatArrays: [[1, 2], [3, 4]] },
          sliced: { $slice: [[10, 20, 30, 40], 1, 2] },
          ranged: { $range: [0, 5] },
          contains: { $in: ['cat', ['dog', 'cat', 'bird']] },
          isArr: { $isArray: [[1, 2, 3]] },
          firstEl: { $first: [[10, 20, 30]] },
          lastEl: { $last: [[10, 20, 30]] },
          reversed: { $reverseArray: [[1, 2, 3]] },
        },
      },
    ]).toArray(),
  );
  assert.deepEqual(arrayMath, [
    {
      summed: 10,
      summedField: 2,
      averaged: 20,
      minVal: 1,
      maxVal: 3,
      power: 256,
      rooted: 4,
      logBase: 2,
      merged: [1, 2, 3, 4],
      sliced: [20, 30],
      ranged: [0, 1, 2, 3, 4],
      contains: true,
      isArr: true,
      firstEl: 10,
      lastEl: 30,
      reversed: [3, 2, 1],
    },
  ]);
  console.log('  array+math:', JSON.stringify(arrayMath));

  const addFieldsAgg = await safe('aggregate $set+$unset+$count', () =>
    agg.aggregate([
      { $set: { renamedKind: '$kind', seen: true } },
      { $unset: ['kind', 'score'] },
      { $match: { renamedKind: 'cat' } },
      { $count: 'totalCats' },
    ]).toArray(),
  );
  assert.deepEqual(addFieldsAgg, [{ totalCats: 2 }]);
  console.log('  set/unset/count:', JSON.stringify(addFieldsAgg));

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
  assert.deepEqual(arrayExpressions, [
    { firstItem: 'cat', bundleSize: 2, maybeScore: 2, literalDollar: '$score' },
    { firstItem: 'cat', bundleSize: 2, maybeScore: 5, literalDollar: '$score' },
    { firstItem: 'dog', bundleSize: 2, maybeScore: 3, literalDollar: '$score' },
  ]);
  console.log('  array expressions:', JSON.stringify(arrayExpressions));

  const objectHelpers = await safe('aggregate object helpers', () =>
    agg.aggregate([
      { $sort: { score: 1 } },
      { $limit: 1 },
      {
        $project: {
          _id: 0,
          mergedDoc: {
            $mergeObjects: [
              { base: true },
              { kind: '$kind' },
              { base: false, score: '$score' },
            ],
          },
          kvPairs: { $objectToArray: { kind: '$kind', score: '$score' } },
          rebuiltDoc: {
            $arrayToObject: [[
              { k: 'kind', v: '$kind' },
              { k: 'score', v: '$score' },
            ]],
          },
          fetchedKind: { $getField: 'kind' },
          dottedLiteralKey: {
            $getField: {
              field: 'odd.name',
              input: { $literal: { 'odd.name': 2 } },
            },
          },
        },
      },
    ]).toArray(),
  );
  assert.deepEqual(objectHelpers, [
    {
      mergedDoc: { base: false, kind: 'cat', score: 2 },
      kvPairs: [
        { k: 'kind', v: 'cat' },
        { k: 'score', v: 2 },
      ],
      rebuiltDoc: { kind: 'cat', score: 2 },
      fetchedKind: 'cat',
      dottedLiteralKey: 2,
    },
  ]);
  console.log('  object helpers:', JSON.stringify(objectHelpers));

  const sortByCountAgg = await safe('aggregate $sortByCount', () =>
    agg.aggregate([
      { $sortByCount: '$kind' },
    ]).toArray(),
  );
  assert.deepEqual(sortByCountAgg, [
    { _id: 'cat', count: 2 },
    { _id: 'dog', count: 1 },
  ]);
  console.log('  sortByCount:', JSON.stringify(sortByCountAgg));

  const aggregateCursor = await safe('aggregate cursor getMore', async () => {
    const cursor = agg.aggregate(
      [{ $sort: { score: 1 } }],
      { cursor: { batchSize: 1 } },
    );
    const docs = await cursor.toArray();
    return docs;
  });
  assert.deepEqual(
    aggregateCursor.map((doc) => [doc.kind, doc.score]),
    [['cat', 2], ['dog', 3], ['cat', 5]],
  );
  console.log('  aggregate cursor total:', aggregateCursor.length);

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
  assert.deepEqual(unwindOptions, [
    { name: 'alpha', tags: 'x', tagIndex: 0 },
    { name: 'alpha', tags: 'y', tagIndex: 1 },
    { name: 'beta', tagIndex: null },
    { name: 'gamma', tagIndex: null },
  ]);
  console.log('  unwind options:', JSON.stringify(unwindOptions));

  const owners = zoo.collection('owners');
  const pets = zoo.collection('pets');
  await safe('insertMany owners', () =>
    owners.insertMany([
      { ownerId: 1, name: 'alice' },
      { ownerId: 2, name: 'bob' },
    ]),
  );
  await safe('insertMany pets', () =>
    pets.insertMany([
      { ownerId: 1, name: 'milo' },
      { ownerId: 3, name: 'ghost' },
    ]),
  );
  const joined = await safe('aggregate $lookup+$unwind', () =>
    owners.aggregate([
      {
        $lookup: {
          from: 'pets',
          localField: 'ownerId',
          foreignField: 'ownerId',
          as: 'pets',
        },
      },
      { $unwind: '$pets' },
      { $project: { _id: 0, owner: '$name', pet: '$pets.name' } },
    ]).toArray(),
  );
  assert.deepEqual(joined, [{ owner: 'alice', pet: 'milo' }]);
  console.log('  lookup+unwind:', JSON.stringify(joined));

  const replaced = await safe('aggregate $replaceRoot/$replaceWith', () =>
    owners.aggregate([
      {
        $lookup: {
          from: 'pets',
          localField: 'ownerId',
          foreignField: 'ownerId',
          as: 'pets',
        },
      },
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
  assert.equal(replaced.length, 1);
  assert.equal(replaced[0].petName, 'milo');
  assert.equal(replaced[0].summary, `milo:${String(replaced[0].ownerRef)}`);
  console.log('  replaceRoot/replaceWith:', JSON.stringify(replaced));

  const pipelineLookup = await safe('aggregate $lookup pipeline form', () =>
    owners.aggregate([
      {
        $lookup: {
          from: 'pets',
          let: { ownerId: '$ownerId' },
          pipeline: [
            { $match: { $expr: { $eq: ['$ownerId', '$$ownerId'] } } },
            { $project: { _id: 0, name: 1 } },
          ],
          as: 'pets',
        },
      },
      { $project: { _id: 0, name: 1, pets: 1 } },
      { $sort: { name: 1 } },
    ]).toArray(),
  );
  assert.deepEqual(pipelineLookup.map((doc) => ({
    name: doc.name,
    pets: Array.isArray(doc.pets) ? doc.pets.map((pet) => pet.name) : [],
  })), [
    { name: 'alice', pets: ['milo'] },
    { name: 'bob', pets: [] },
  ]);
  console.log('  lookup pipeline:', JSON.stringify(pipelineLookup));

  const sparseLocal = zoo.collection('lookup_sparse_local');
  const sparseForeign = zoo.collection('lookup_sparse_foreign');
  await safe('insertMany lookup sparse local', () =>
    sparseLocal.insertMany([
      { name: 'missing' },
      { name: 'null', joinKey: null },
      { name: 'value', joinKey: 'x' },
    ]),
  );
  await safe('insertMany lookup sparse foreign', () =>
    sparseForeign.insertMany([
      { label: 'missing' },
      { label: 'null', joinKey: null },
      { label: 'value', joinKey: 'x' },
    ]),
  );
  const sparseLookup = await safe('aggregate sparse $lookup null matching', () =>
    sparseLocal.aggregate([
      {
        $lookup: {
          from: 'lookup_sparse_foreign',
          localField: 'joinKey',
          foreignField: 'joinKey',
          as: 'matches',
        },
      },
      { $project: { _id: 0, name: 1, matches: 1 } },
    ]).toArray(),
  );
  const lookupMissing = sparseLookup.find((doc) => doc.name === 'missing');
  const lookupNull = sparseLookup.find((doc) => doc.name === 'null');
  const lookupValue = sparseLookup.find((doc) => doc.name === 'value');
  assert.deepEqual(
    lookupMissing.matches.map((doc) => doc.label).sort(),
    ['missing', 'null'],
  );
  assert.deepEqual(
    lookupNull.matches.map((doc) => doc.label).sort(),
    ['missing', 'null'],
  );
  assert.deepEqual(
    lookupValue.matches.map((doc) => doc.label),
    ['value'],
  );
  console.log('  sparse lookup:', JSON.stringify(sparseLookup));
}
