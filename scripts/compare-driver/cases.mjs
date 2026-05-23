function pickHelloShape(doc) {
  return {
    ok: doc?.ok === 1,
    isWritablePrimary:
      doc?.isWritablePrimary === true || doc?.ismaster === true || doc?.isMaster === true,
    hasTopologyVersion:
      !!doc?.topologyVersion &&
      typeof doc.topologyVersion === 'object' &&
      doc.topologyVersion !== null,
    hasSessionTimeout: typeof doc?.logicalSessionTimeoutMinutes === 'number',
    hasConnectionId: typeof doc?.connectionId === 'number',
    hasMaxBsonObjectSize: typeof doc?.maxBsonObjectSize === 'number',
    hasMaxMessageSizeBytes: typeof doc?.maxMessageSizeBytes === 'number',
    hasMaxWriteBatchSize: typeof doc?.maxWriteBatchSize === 'number',
    minWireVersionIsNumber: typeof doc?.minWireVersion === 'number',
    maxWireVersionAtLeast17:
      typeof doc?.maxWireVersion === 'number' && doc.maxWireVersion >= 17,
  };
}

function pickBuildInfoShape(doc) {
  return {
    ok: doc?.ok === 1,
    hasVersion: typeof doc?.version === 'string' && doc.version.length > 0,
    hasGitVersion: typeof doc?.gitVersion === 'string',
    hasVersionArray: Array.isArray(doc?.versionArray),
    bitsIsNumber: typeof doc?.bits === 'number',
    debugIsBoolean: typeof doc?.debug === 'boolean',
  };
}

function sortDocsById(docs) {
  return docs
    .slice()
    .sort((left, right) => String(left._id).localeCompare(String(right._id)));
}

function projectFields(docs, fields) {
  return docs.map((doc) =>
    Object.fromEntries(fields.filter((field) => field in doc).map((field) => [field, doc[field]])),
  );
}

function normalizeNullishTail(labels, nonNullishCount) {
  return {
    orderedPrefix: labels.slice(0, nonNullishCount),
    nullishBag: labels
      .slice(nonNullishCount)
      .slice()
      .sort((left, right) => left.localeCompare(right)),
  };
}

export function makeCases() {
  return [
    {
      name: 'hello-shape',
      expectation: 'match',
      tags: ['wire', 'admin', 'hello'],
      featureRefs: ['command:hello'],
      run: async ({ admin }) => pickHelloShape(await admin.command({ hello: 1 })),
    },
    {
      name: 'ping-shape',
      expectation: 'match',
      tags: ['wire', 'admin', 'ping'],
      featureRefs: ['command:ping'],
      run: async ({ admin }) => ({ ok: (await admin.command({ ping: 1 })).ok === 1 }),
    },
    {
      name: 'buildinfo-shape',
      expectation: 'match',
      tags: ['wire', 'admin', 'buildinfo'],
      featureRefs: ['command:buildInfo'],
      run: async ({ admin }) => pickBuildInfoShape(await admin.command({ buildInfo: 1 })),
    },
    {
      name: 'index-create-list-drop',
      expectation: 'match',
      tags: ['wire', 'index', 'subcommand'],
      featureRefs: ['command:createIndexes', 'command:listIndexes', 'command:dropIndexes'],
      run: async ({ db }) => {
        const coll = db.collection('index_cases');
        await coll.insertMany([
          { _id: 'a', value: 1 },
          { _id: 'b', value: 2 },
        ]);
        await coll.createIndex({ value: 1 }, { name: 'value_1' });
        const created = (await coll.listIndexes().toArray())
          .filter((entry) => entry.name === 'value_1')
          .map((entry) => ({ name: entry.name, key: entry.key }));
        await coll.dropIndex('value_1');
        const afterDrop = (await coll.listIndexes().toArray()).filter(
          (entry) => entry.name === 'value_1',
        ).length;
        return {
          created,
          afterDrop,
        };
      },
    },
    {
      name: 'find-filter-projection-sort-window',
      expectation: 'match',
      tags: ['find', 'query', 'projection', 'sort'],
      featureRefs: ['command:find', 'query-op:$eq'],
      run: async ({ db }) => {
        const coll = db.collection('find_cases');
        await coll.insertMany([
          { _id: '1', city: { name: 'NYC' }, pop: 8000, kind: 'metro' },
          { _id: '2', city: { name: 'SF' }, pop: 900, kind: 'metro' },
          { _id: '3', city: { name: 'LA' }, pop: 4000, kind: 'metro' },
        ]);
        const nyc = await coll.find({ 'city.name': 'NYC' }).project({ _id: 0, pop: 1 }).toArray();
        const window = await coll
          .find({}, { projection: { _id: 0, city: 1, pop: 1 } })
          .sort({ pop: -1 })
          .skip(1)
          .limit(1)
          .toArray();
        return {
          nyc,
          window: window.map((doc) => ({ city: doc.city.name, pop: doc.pop })),
        };
      },
    },
    {
      name: 'find-mixed-type-sort',
      expectation: 'match',
      tags: ['find', 'sort', 'mixed-types'],
      featureRefs: ['command:find'],
      run: async ({ db }) => {
        const coll = db.collection('mixed_sort_cases');
        await coll.insertMany([
          { _id: 'missing', label: 'missing' },
          { _id: 'null', label: 'null', v: null },
          { _id: 'number', label: 'number', v: 5 },
          { _id: 'string', label: 'string', v: 'zzz' },
          { _id: 'bool', label: 'bool', v: true },
        ]);
        const asc = await coll
          .find({}, { projection: { _id: 0, label: 1 } })
          .sort({ v: 1 })
          .toArray();
        const desc = await coll
          .find({}, { projection: { _id: 0, label: 1 } })
          .sort({ v: -1 })
          .toArray();
        return {
          asc: normalizeNullishTail(
            asc.map((doc) => doc.label),
            3,
          ),
          desc: normalizeNullishTail(
            desc.map((doc) => doc.label),
            3,
          ),
        };
      },
    },
    {
      name: 'find-indexed-sparse-sort',
      expectation: 'match',
      tags: ['find', 'sort', 'index', 'mixed-types'],
      featureRefs: ['command:find', 'command:createIndexes'],
      run: async ({ db }) => {
        const coll = db.collection('indexed_sparse_sort_cases');
        await coll.insertMany([
          { _id: 'missing-a', label: 'missing-a' },
          { _id: 'null-a', label: 'null-a', v: null },
          { _id: 'missing-b', label: 'missing-b' },
          { _id: 'null-b', label: 'null-b', v: null },
          { _id: 'number', label: 'number', v: 5 },
          { _id: 'string', label: 'string', v: 'zzz' },
          { _id: 'bool', label: 'bool', v: true },
        ]);
        await coll.createIndex({ v: 1 }, { name: 'v_1' });
        const asc = await coll
          .find({}, { projection: { _id: 0, label: 1 } })
          .sort({ v: 1 })
          .toArray();
        const desc = await coll
          .find({}, { projection: { _id: 0, label: 1 } })
          .sort({ v: -1 })
          .toArray();
        return {
          asc: normalizeNullishTail(
            asc.map((doc) => doc.label),
            3,
          ),
          desc: normalizeNullishTail(
            desc.map((doc) => doc.label),
            3,
          ),
        };
      },
    },
    {
      name: 'update-delete-upsert-state',
      expectation: 'match',
      tags: ['update', 'delete', 'write'],
      featureRefs: [
        'command:update',
        'command:delete',
        'update-op:$set',
        'update-op:$inc',
      ],
      run: async ({ db }) => {
        const coll = db.collection('mut_cases');
        await coll.insertMany([
          { _id: '1', n: 1, hits: 0 },
          { _id: '2', n: 2, hits: 0 },
          { _id: '3', n: 3, hits: 0 },
          { _id: '4', n: 4, hits: 0 },
        ]);
        const setResult = await coll.updateOne({ _id: '1' }, { $set: { name: 'first' } });
        const incResult = await coll.updateMany({}, { $inc: { hits: 1 } });
        const upsertResult = await coll.updateOne(
          { _id: 'upserted' },
          { $set: { n: 999, label: 'inserted-by-upsert' } },
          { upsert: true },
        );
        const replaceResult = await coll.replaceOne({ _id: '1' }, { n: 1, replaced: true });
        const deleteOneResult = await coll.deleteOne({ _id: '2' });
        const deleteManyResult = await coll.deleteMany({ hits: 1, _id: { $ne: '1' } });
        const finalDocs = sortDocsById(await coll.find({}).toArray());
        return {
          counts: {
            setMatched: setResult.matchedCount,
            setModified: setResult.modifiedCount,
            incMatched: incResult.matchedCount,
            incModified: incResult.modifiedCount,
            upsertedCount: upsertResult.upsertedCount,
            replaceMatched: replaceResult.matchedCount,
            replaceModified: replaceResult.modifiedCount,
            deleteOne: deleteOneResult.deletedCount,
            deleteMany: deleteManyResult.deletedCount,
          },
          finalDocs: projectFields(finalDocs, ['_id', 'n', 'hits', 'replaced', 'label']),
        };
      },
    },
    {
      name: 'aggregate-group-and-sparse-semantics',
      expectation: 'match',
      tags: ['aggregation', 'group'],
      featureRefs: [
        'command:aggregate',
        'agg-stage:$match',
        'agg-stage:$group',
        'agg-stage:$sort',
        'agg-stage:$sortByCount',
        'agg-accum:$sum',
        'agg-accum:$avg',
        'agg-accum:$min',
        'agg-accum:$max',
        'agg-accum:$first',
        'agg-accum:$last',
        'agg-accum:$push',
      ],
      run: async ({ db }) => {
        const coll = db.collection('agg_group_cases');
        await coll.insertMany([
          { _id: '1', kind: 'cat', score: 2 },
          { _id: '2', kind: 'cat', score: 5 },
          { _id: '3', kind: 'dog', score: 3 },
          { _id: '4', value: 2 },
          { _id: '5', bucket: 'a', value: 1 },
          { _id: '6', bucket: 'a' },
          { _id: '7' },
        ]);
        const grouped = await coll
          .aggregate([
            { $match: { kind: { $in: ['cat', 'dog'] } } },
            {
              $group: {
                _id: '$kind',
                minScore: { $min: '$score' },
                maxScore: { $max: '$score' },
                avgScore: { $avg: '$score' },
                total: { $sum: 1 },
              },
            },
            { $sort: { _id: 1 } },
          ])
          .toArray();
        const sparse = await coll
          .aggregate([
            {
              $match: {
                $or: [{ bucket: 'a' }, { value: { $exists: true } }, { _id: '7' }],
              },
            },
            {
              $group: {
                _id: '$bucket',
                firstValue: { $first: '$value' },
                lastValue: { $last: '$value' },
                values: { $push: '$value' },
                count: { $sum: 1 },
              },
            },
            { $sort: { _id: 1 } },
          ])
          .toArray();
        const sortByCount = await coll
          .aggregate([{ $match: { kind: { $exists: true } } }, { $sortByCount: '$kind' }])
          .toArray();
        return { grouped, sparse, sortByCount };
      },
    },
    {
      name: 'aggregate-expression-families',
      expectation: 'match',
      tags: ['aggregation', 'expressions'],
      featureRefs: [
        'command:aggregate',
        'agg-stage:$project',
        'agg-expr:$concat',
        'agg-expr:$toString',
        'agg-expr:$ifNull',
        'agg-expr:$multiply',
        'agg-expr:$add',
        'agg-expr:$divide',
        'agg-expr:$mod',
        'agg-expr:$subtract',
        'agg-expr:$abs',
        'agg-expr:$round',
        'agg-expr:$cond',
        'agg-expr:$switch',
        'agg-expr:$and',
        'agg-expr:$or',
        'agg-expr:$not',
        'agg-expr:$toUpper',
        'agg-expr:$toLower',
        'agg-expr:$trim',
        'agg-expr:$substr',
        'agg-expr:$split',
        'agg-expr:$strLenCP',
        'agg-expr:$type',
        'agg-expr:$toInt',
        'agg-expr:$toDouble',
        'agg-expr:$toBool',
        'agg-expr:$isNumber',
      ],
      run: async ({ db }) => {
        const coll = db.collection('agg_expr_cases');
        await coll.insertMany([
          { _id: '1', kind: 'cat', score: 2 },
          { _id: '2', kind: 'dog', score: 3 },
          { _id: '3', kind: 'cat', score: 5 },
        ]);
        return coll
          .aggregate([
            { $sort: { score: 1 } },
            {
              $project: {
                _id: 0,
                kind: 1,
                score: 1,
                label: { $concat: ['$kind', ':', { $toString: '$score' }] },
                fallback: { $ifNull: ['$missing', 'fallback'] },
                doubled: { $multiply: ['$score', 2] },
                plusOne: { $add: ['$score', 1] },
                half: { $divide: ['$score', 2] },
                remainder: { $mod: ['$score', 2] },
                delta: { $subtract: ['$score', 3] },
                absDelta: { $abs: { $subtract: ['$score', 3] } },
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
                upper: { $toUpper: '$kind' },
                lower: { $toLower: '$kind' },
                trimmed: { $trim: { input: '  hi  ' } },
                first3: { $substr: ['$kind', 0, 3] },
                parts: { $split: ['a,b,c', ','] },
                len: { $strLenCP: '$kind' },
                scoreType: { $type: '$score' },
                parsedInt: { $toInt: '42' },
                parsedDouble: { $toDouble: '3.14' },
                boolFromInt: { $toBool: '$score' },
                isScoreNum: { $isNumber: '$score' },
              },
            },
          ])
          .toArray();
      },
    },
    {
      name: 'aggregate-array-and-object-helpers',
      expectation: 'match',
      tags: ['aggregation', 'arrays', 'objects'],
      featureRefs: [
        'command:aggregate',
        'agg-stage:$addFields',
        'agg-expr:$arrayElemAt',
        'agg-expr:$size',
        'agg-expr:$literal',
        'agg-expr:$pow',
        'agg-expr:$sqrt',
        'agg-expr:$log',
        'agg-expr:$concatArrays',
        'agg-expr:$slice',
        'agg-expr:$range',
        'agg-expr:$in',
        'agg-expr:$isArray',
        'agg-expr:$first',
        'agg-expr:$last',
        'agg-expr:$reverseArray',
        'agg-expr:$mergeObjects',
        'agg-expr:$objectToArray',
        'agg-expr:$arrayToObject',
        'agg-expr:$getField',
      ],
      run: async ({ db }) => {
        const coll = db.collection('agg_array_object_cases');
        await coll.insertOne({ _id: '1', kind: 'cat', score: 2 });
        return coll
          .aggregate([
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
          ])
          .toArray();
      },
    },
    {
      name: 'aggregate-unwind-and-lookup',
      expectation: 'match',
      tags: ['aggregation', 'lookup', 'unwind'],
      featureRefs: [
        'command:aggregate',
        'agg-stage:$lookup',
        'agg-stage:$unwind',
        'agg-stage:$replaceRoot',
        'agg-stage:$replaceWith',
      ],
      run: async ({ db }) => {
        const unwindColl = db.collection('unwind_cases');
        await unwindColl.insertMany([
          { _id: '1', name: 'alpha', tags: ['x', 'y'] },
          { _id: '2', name: 'beta', tags: [] },
          { _id: '3', name: 'gamma' },
        ]);
        const unwind = await unwindColl
          .aggregate([
            {
              $unwind: {
                path: '$tags',
                preserveNullAndEmptyArrays: true,
                includeArrayIndex: 'tagIndex',
              },
            },
            { $project: { _id: 0, name: 1, tags: 1, tagIndex: 1 } },
            { $sort: { name: 1, tagIndex: 1 } },
          ])
          .toArray();

        const owners = db.collection('owners');
        const pets = db.collection('pets');
        await owners.insertMany([
          { _id: 'owner-a', name: 'alice' },
          { _id: 'owner-b', name: 'bob' },
          { _id: 'owner-c', name: 'nobody' },
        ]);
        await pets.insertMany([
          { _id: 'pet-a', ownerId: 'owner-a', name: 'milo' },
          { _id: 'pet-b', ownerId: 'owner-a', name: 'otis' },
          { _id: 'pet-c', ownerId: 'owner-b', name: 'luna' },
        ]);

        const joined = await owners
          .aggregate([
            { $lookup: { from: 'pets', localField: '_id', foreignField: 'ownerId', as: 'pets' } },
            { $unwind: '$pets' },
            { $match: { 'pets.name': 'milo' } },
            { $project: { _id: 0, owner: '$name', pet: '$pets.name' } },
          ])
          .toArray();

        const replaced = await owners
          .aggregate([
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
          ])
          .toArray();

        return { unwind, joined, replaced };
      },
    },
    {
      name: 'aggregate-sparse-lookup-null-matching',
      expectation: 'match',
      tags: ['aggregation', 'lookup', 'nulls'],
      featureRefs: ['command:aggregate', 'agg-stage:$lookup'],
      run: async ({ db }) => {
        const local = db.collection('lookup_sparse_local');
        const foreign = db.collection('lookup_sparse_foreign');
        await local.insertMany([
          { _id: 'a', name: 'missing' },
          { _id: 'b', name: 'null', joinKey: null },
          { _id: 'c', name: 'value', joinKey: 'x' },
        ]);
        await foreign.insertMany([
          { _id: 'f1', label: 'missing' },
          { _id: 'f2', label: 'null', joinKey: null },
          { _id: 'f3', label: 'value', joinKey: 'x' },
        ]);
        const results = await local
          .aggregate([
            {
              $lookup: {
                from: 'lookup_sparse_foreign',
                localField: 'joinKey',
                foreignField: 'joinKey',
                as: 'matches',
              },
            },
            { $project: { _id: 0, name: 1, matches: 1 } },
            { $sort: { name: 1 } },
          ])
          .toArray();
        return results.map((doc) => ({
          name: doc.name,
          matches: Array.isArray(doc.matches)
            ? doc.matches.map((match) => match.label).sort((left, right) => left.localeCompare(right))
            : [],
        }));
      },
    },
    {
      name: 'listcommands-core-surface',
      expectation: 'match',
      tags: ['wire', 'subcommand', 'admin'],
      featureRefs: ['command:listCommands'],
      run: async ({ db }) => {
        const result = await db.command({ listCommands: 1 });
        const core = ['aggregate', 'find', 'insert', 'update', 'delete'].filter(
          (name) => result?.commands?.[name],
        );
        return { core };
      },
    },
    {
      name: 'distinct-basic-values',
      expectation: 'match',
      tags: ['query', 'subcommand', 'distinct'],
      featureRefs: ['command:distinct'],
      run: async ({ db }) => {
        const coll = db.collection('distinct_cases');
        await coll.insertMany([
          { _id: '1', kind: 'cat' },
          { _id: '2', kind: 'cat' },
          { _id: '3', kind: 'dog' },
        ]);
        return {
          values: (await coll.distinct('kind')).slice().sort((left, right) => left.localeCompare(right)),
        };
      },
    },
    {
      name: 'lookup-pipeline-form',
      expectation: 'match',
      tags: ['aggregation', 'lookup'],
      featureRefs: ['command:aggregate', 'agg-stage:$lookup'],
      run: async ({ db }) => {
        const owners = db.collection('lookup_pipe_owners');
        const pets = db.collection('lookup_pipe_pets');
        await owners.insertMany([
          { _id: 'owner-a', name: 'alice' },
          { _id: 'owner-b', name: 'bob' },
        ]);
        await pets.insertMany([
          { _id: 'pet-a', ownerId: 'owner-a', name: 'milo' },
          { _id: 'pet-b', ownerId: 'owner-a', name: 'otis' },
          { _id: 'pet-c', ownerId: 'owner-b', name: 'luna' },
        ]);
        return owners
          .aggregate([
            {
              $lookup: {
                from: 'lookup_pipe_pets',
                let: { ownerId: '$_id' },
                pipeline: [
                  { $match: { $expr: { $eq: ['$ownerId', '$$ownerId'] } } },
                  { $project: { _id: 0, name: 1 } },
                ],
                as: 'pets',
              },
            },
            { $project: { _id: 0, name: 1, pets: 1 } },
            { $sort: { name: 1 } },
          ])
          .toArray()
          .then((docs) =>
            docs.map((doc) => ({
              name: doc.name,
              pets: Array.isArray(doc.pets) ? doc.pets.map((pet) => pet.name) : [],
            })),
          );
      },
    },
  ];
}
