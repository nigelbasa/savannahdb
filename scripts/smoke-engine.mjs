// Load the native addon directly and exercise insert/find/getMore/killCursors.
import { createRequire } from 'node:module';
import { BSON } from 'bson';
import assert from 'node:assert/strict';

const require = createRequire(import.meta.url);
const engine = require('../build/Release/savannah_engine.node');

assert.equal(typeof engine.insert, 'function', 'engine.insert missing');
assert.equal(typeof engine.createIndex, 'function', 'engine.createIndex missing');
assert.equal(typeof engine.dropIndex, 'function', 'engine.dropIndex missing');
assert.equal(typeof engine.listIndexes, 'function', 'engine.listIndexes missing');
assert.equal(typeof engine.find, 'function', 'engine.find missing');
assert.equal(typeof engine.getMore, 'function', 'engine.getMore missing');
assert.equal(typeof engine.killCursors, 'function', 'engine.killCursors missing');

// Helper: drain a query in one shot. Most pre-Slice-D assertions don't care
// about pagination, so we use a huge batchSize and assert the cursor closed.
const HUGE = 1_000_000;
const EMPTY = Buffer.from([5, 0, 0, 0, 0]);
function findAll(db, coll, filter) {
  const { batch, cursorId } = engine.find(db, coll, filter, HUGE, EMPTY, 0, 0, EMPTY);
  assert.equal(cursorId, 0n, 'expected exhausted cursor for findAll');
  return batch;
}

function getIndexInfo(db, coll, name) {
  return engine.listIndexes(db, coll).find((index) => index.name === name);
}

function assertByteIdentical(actual, expected, label) {
  assert.equal(actual.length, expected.length, `${label}: length mismatch`);
  for (let i = 0; i < actual.length; ++i) {
    assert.equal(
      Buffer.compare(Buffer.from(actual[i]), Buffer.from(expected[i])),
      0,
      `${label}: doc ${i} differs`,
    );
  }
}
const ser = (o) => Buffer.from(BSON.serialize(o));

const docs = [
  ser({ _id: 1, name: 'giraffe', height: 5.5 }),
  ser({ _id: 2, name: 'lion', pack: ['simba', 'nala'] }),
  ser({ _id: 3, name: 'elephant', age: 42 }),
];

const insertRes = engine.insert('zoo', 'animals', docs);
console.log('insert ->', insertRes);
assert.equal(insertRes.insertedCount, 3);

const found = findAll('zoo', 'animals', Buffer.alloc(5));
console.log(`find -> ${found.length} docs`);
assert.equal(found.length, 3);

const decoded = found.map((b) => BSON.deserialize(b));
for (const d of decoded) console.log(' ', JSON.stringify(d));
assert.equal(decoded[0].name, 'giraffe');
assert.equal(decoded[1].pack[1], 'nala');
assert.equal(decoded[2].age, 42);

engine.insert('zoo', 'animals', [ser({ _id: 4, name: 'zebra' })]);
assert.equal(findAll('zoo', 'animals', Buffer.alloc(5)).length, 4);
assert.equal(findAll('zoo', 'plants', Buffer.alloc(5)).length, 0);

// ---- C1b: top-level equality filtering -------------------------------
const byName = findAll('zoo', 'animals', ser({ name: 'lion' })).map((b) =>
  BSON.deserialize(b),
);
assert.equal(byName.length, 1);
assert.equal(byName[0]._id, 2);

assert.equal(findAll('zoo', 'animals', ser({ name: 'penguin' })).length, 0);

engine.insert('zoo', 'animals', [
  ser({ _id: 5, name: 'cheetah', speed: 75 }),
  ser({ _id: 6, name: 'sloth', speed: 0.2 }),
]);
const fast = findAll('zoo', 'animals', ser({ speed: 75 }));
assert.equal(fast.length, 1);
assert.equal(BSON.deserialize(fast[0]).name, 'cheetah');

const byId = findAll('zoo', 'animals', ser({ _id: 3 })).map((b) =>
  BSON.deserialize(b),
);
assert.equal(byId.length, 1);
assert.equal(byId[0].name, 'elephant');

assert.equal(findAll('zoo', 'animals', ser({})).length, 6);

// ---- C2: comparison operators ---------------------------------------
const eqLion = findAll('zoo', 'animals', ser({ name: { $eq: 'lion' } })).map(
  (b) => BSON.deserialize(b),
);
assert.equal(eqLion.length, 1);
assert.equal(eqLion[0]._id, 2);

engine.insert('zoo', 'animals', [
  ser({ _id: 7, name: 'spider', meta: { legs: 8 } }),
]);
const eqSubdoc = findAll('zoo', 'animals', ser({ meta: { $eq: { legs: 8 } } }));
assert.equal(eqSubdoc.length, 1);
assert.equal(BSON.deserialize(eqSubdoc[0]).name, 'spider');

const neAge = findAll('zoo', 'animals', ser({ age: { $ne: 99 } })).map((b) =>
  BSON.deserialize(b),
);
assert.ok(neAge.length >= 6);
assert.ok(neAge.some((d) => d.name === 'lion'));
assert.ok(neAge.some((d) => d.name === 'elephant'));

const neAge42 = findAll('zoo', 'animals', ser({ age: { $ne: 42 } })).map((b) =>
  BSON.deserialize(b),
);
assert.ok(!neAge42.some((d) => d.name === 'elephant'));
assert.ok(neAge42.some((d) => d.name === 'lion'));

engine.insert('zoo', 'animals', [
  ser({ _id: 8, name: 'goose', n: 7 }),
  ser({ _id: 9, name: 'duck', n: 4 }),
  ser({ _id: 10, name: 'swan', n: 11 }),
]);
const between = findAll('zoo', 'animals', ser({ n: { $gt: 5, $lt: 10 } })).map(
  (b) => BSON.deserialize(b),
);
assert.equal(between.length, 1);
assert.equal(between[0].name, 'goose');

const gteSeven = findAll('zoo', 'animals', ser({ n: { $gte: 7 } }));
assert.equal(gteSeven.length, 2);

const crossType = findAll('zoo', 'animals', ser({ name: { $gt: 5 } }));
assert.equal(crossType.length, 0);

// ---- C3: set + logical operators ------------------------------------
const cats = findAll('zoo', 'animals', ser({ name: { $in: ['lion', 'cheetah'] } }));
assert.equal(cats.length, 2);

const ninNames = findAll(
  'zoo',
  'animals',
  ser({ name: { $nin: ['lion', 'goose'] } }),
).map((b) => BSON.deserialize(b));
assert.ok(!ninNames.some((d) => d.name === 'lion'));
assert.ok(!ninNames.some((d) => d.name === 'goose'));
assert.ok(ninNames.some((d) => d.name === 'giraffe'));

const hasAge = findAll('zoo', 'animals', ser({ age: { $exists: true } })).map(
  (b) => BSON.deserialize(b),
);
assert.equal(hasAge.length, 1);
assert.equal(hasAge[0].name, 'elephant');

const noAge = findAll('zoo', 'animals', ser({ age: { $exists: false } })).map(
  (b) => BSON.deserialize(b),
);
assert.ok(noAge.length >= 6);
assert.ok(!noAge.some((d) => d.name === 'elephant'));

const orRes = findAll(
  'zoo',
  'animals',
  ser({ $or: [{ name: 'lion' }, { name: 'spider' }] }),
);
assert.equal(orRes.length, 2);

const andRes = findAll(
  'zoo',
  'animals',
  ser({ $and: [{ name: 'goose' }, { n: { $gt: 5 } }] }),
).map((b) => BSON.deserialize(b));
assert.equal(andRes.length, 1);
assert.equal(andRes[0].name, 'goose');

const orPartial = findAll(
  'zoo',
  'animals',
  ser({ $or: [{ name: 'penguin' }, { _id: 1 }] }),
).map((b) => BSON.deserialize(b));
assert.equal(orPartial.length, 1);
assert.equal(orPartial[0].name, 'giraffe');

// ---- C4: $regex ------------------------------------------------------
const eRes = findAll('zoo', 'animals', ser({ name: { $regex: 'e' } })).map(
  (b) => BSON.deserialize(b),
);
const eNames = eRes.map((d) => d.name).sort();
assert.ok(eNames.includes('giraffe'));
assert.ok(eNames.includes('elephant'));
assert.ok(eNames.includes('cheetah'));
assert.ok(!eNames.includes('lion'));

assert.equal(findAll('zoo', 'animals', ser({ name: { $regex: 'LION' } })).length, 0);
const insensitive = findAll(
  'zoo',
  'animals',
  ser({ name: { $regex: 'LION', $options: 'i' } }),
).map((b) => BSON.deserialize(b));
assert.equal(insensitive.length, 1);
assert.equal(insensitive[0].name, 'lion');

const literal = findAll('zoo', 'animals', ser({ name: /^gir/ })).map((b) =>
  BSON.deserialize(b),
);
assert.equal(literal.length, 1);

const anchored = findAll('zoo', 'animals', ser({ name: { $regex: '^s' } })).map(
  (b) => BSON.deserialize(b),
);
const anchoredNames = anchored.map((d) => d.name).sort();
assert.deepEqual(anchoredNames, ['sloth', 'spider', 'swan']);

assert.equal(
  findAll('zoo', 'animals', ser({ nope: { $regex: '.' } })).length,
  0,
);

// ---- Slice D: cursor registry, getMore, killCursors -----------------
// Use a fresh collection so the document count is deterministic.
for (let i = 1; i <= 5; ++i) {
  engine.insert('zoo', 'paginate', [ser({ _id: i, n: i })]);
}

// Test 1: 5 matches, batchSize=2 → 2/2/1 with id closing on the last batch.
{
  const r1 = engine.find('zoo', 'paginate', ser({}), 2, EMPTY, 0, 0, EMPTY);
  assert.equal(r1.batch.length, 2);
  assert.notEqual(r1.cursorId, 0n, 'first batch should leave cursor open');

  const r2 = engine.getMore(r1.cursorId, 'zoo', 'paginate', 2);
  assert.equal(r2.batch.length, 2);
  assert.equal(r2.cursorId, r1.cursorId, 'cursor id must be stable');

  const r3 = engine.getMore(r1.cursorId, 'zoo', 'paginate', 2);
  assert.equal(r3.batch.length, 1);
  assert.equal(r3.cursorId, 0n, 'final batch must close the cursor');

  // After the cursor closes the registry should drop it.
  assert.throws(
    () => engine.getMore(r1.cursorId, 'zoo', 'paginate', 2),
    /CursorNotFound/,
  );
}

// Test 2: drain-then-peek — exactly batchSize matches must close immediately.
{
  const r = engine.find('zoo', 'paginate', ser({}), 5, EMPTY, 0, 0, EMPTY);
  assert.equal(r.batch.length, 5);
  assert.equal(r.cursorId, 0n, 'no extra round-trip when batch perfectly fits');
}

// Test 3: getMore on unknown id → CursorNotFound.
assert.throws(
  () => engine.getMore(99999999n, 'zoo', 'paginate', 10),
  /CursorNotFound/,
);

// Test 4: getMore with mismatched collection → CursorNotFound.
{
  const r = engine.find('zoo', 'paginate', ser({}), 1, EMPTY, 0, 0, EMPTY);
  assert.notEqual(r.cursorId, 0n);
  assert.throws(
    () => engine.getMore(r.cursorId, 'zoo', 'animals', 10),
    /CursorNotFound/,
  );
  // Clean up the still-live cursor we just opened.
  const k = engine.killCursors([r.cursorId]);
  assert.equal(k.killed.length, 1);
}

// Test 5: killCursors twice — second call reports notFound.
{
  const r = engine.find('zoo', 'paginate', ser({}), 1, EMPTY, 0, 0, EMPTY);
  assert.notEqual(r.cursorId, 0n);
  const k1 = engine.killCursors([r.cursorId]);
  assert.equal(k1.killed.length, 1);
  assert.equal(k1.notFound.length, 0);
  const k2 = engine.killCursors([r.cursorId]);
  assert.equal(k2.killed.length, 0);
  assert.equal(k2.notFound.length, 1);
}

// Test 6: snapshot semantics — inserts during pagination are invisible.
{
  const r1 = engine.find('zoo', 'paginate', ser({}), 2, EMPTY, 0, 0, EMPTY);
  assert.equal(r1.batch.length, 2);
  assert.notEqual(r1.cursorId, 0n);

  // Insert a new doc AFTER the cursor was created.
  engine.insert('zoo', 'paginate', [ser({ _id: 999, n: 999 })]);

  const r2 = engine.getMore(r1.cursorId, 'zoo', 'paginate', 100);
  // The cursor was opened when the collection had 5 docs, so it should
  // return at most the remaining 3 — never the new one.
  const ids = r2.batch.map((b) => BSON.deserialize(b)._id);
  assert.ok(!ids.includes(999), `cursor must not see inserts: got ids ${ids}`);
  assert.equal(r2.cursorId, 0n);
}

// ---- Slice E: update + erase ---------------------------------------
{
  // Fresh collection so doc set is deterministic.
  for (let i = 1; i <= 5; ++i) {
    engine.insert('zoo', 'mut', [ser({ _id: i, name: `n${i}`, hits: i })]);
  }

  // $set on missing field — adds it.
  let r = engine.update('zoo', 'mut', ser({ _id: 1 }), ser({ $set: { tag: 'first' } }), false, false);
  assert.equal(r.matched, 1);
  assert.equal(r.modified, 1);
  assert.equal(r.upsertedIds.length, 0);
  let after = BSON.deserialize(findAll('zoo', 'mut', ser({ _id: 1 }))[0]);
  assert.equal(after.tag, 'first');

  // $set with same value — matched=1, modified=0 (byte-diff catches no-op).
  r = engine.update('zoo', 'mut', ser({ _id: 1 }), ser({ $set: { tag: 'first' } }), false, false);
  assert.equal(r.matched, 1);
  assert.equal(r.modified, 0, 'no-op $set must not bump modified');

  // $inc on existing numeric.
  r = engine.update('zoo', 'mut', ser({ _id: 2 }), ser({ $inc: { hits: 10 } }), false, false);
  assert.equal(r.matched, 1);
  assert.equal(r.modified, 1);
  after = BSON.deserialize(findAll('zoo', 'mut', ser({ _id: 2 }))[0]);
  assert.equal(after.hits, 12);

  // $inc on missing field — creates with the delta.
  r = engine.update('zoo', 'mut', ser({ _id: 1 }), ser({ $inc: { fresh: 5 } }), false, false);
  after = BSON.deserialize(findAll('zoo', 'mut', ser({ _id: 1 }))[0]);
  assert.equal(after.fresh, 5);

  // $inc on non-numeric → TypeMismatch (code 14), no slot mutation.
  r = engine.update('zoo', 'mut', ser({ _id: 1 }), ser({ $inc: { name: 1 } }), false, false);
  assert.equal(r.err?.code, 14, `expected code 14, got ${JSON.stringify(r.err)}`);
  after = BSON.deserialize(findAll('zoo', 'mut', ser({ _id: 1 }))[0]);
  assert.equal(after.name, 'n1', 'failed update must not mutate the slot');

  // $unset removes a field.
  r = engine.update('zoo', 'mut', ser({ _id: 1 }), ser({ $unset: { fresh: '' } }), false, false);
  after = BSON.deserialize(findAll('zoo', 'mut', ser({ _id: 1 }))[0]);
  assert.equal(after.fresh, undefined);

  // multi=true updates every match.
  r = engine.update('zoo', 'mut', ser({}), ser({ $set: { batch: true } }), true, false);
  assert.equal(r.matched, 5);
  const allMut = findAll('zoo', 'mut', ser({})).map((b) => BSON.deserialize(b));
  for (const d of allMut) assert.equal(d.batch, true);

  // multi=false only touches the first match.
  r = engine.update('zoo', 'mut', ser({ batch: true }), ser({ $set: { only: 1 } }), false, false);
  assert.equal(r.matched, 1);
  const onlyMatches = findAll('zoo', 'mut', ser({ only: 1 })).map((b) => BSON.deserialize(b));
  assert.equal(onlyMatches.length, 1);

  // Replacement preserves _id when the spec lacks one.
  r = engine.update('zoo', 'mut', ser({ _id: 3 }), ser({ name: 'replaced', extra: 1 }), false, false);
  assert.equal(r.matched, 1);
  after = BSON.deserialize(findAll('zoo', 'mut', ser({ _id: 3 }))[0]);
  assert.equal(after.name, 'replaced');
  assert.equal(after.extra, 1);
  assert.equal(after.batch, undefined, 'replacement should clear other fields');

  // Replacement with mismatched _id → ImmutableField (code 66).
  r = engine.update('zoo', 'mut', ser({ _id: 3 }), ser({ _id: 999, name: 'no' }), false, false);
  assert.equal(r.err?.code, 66, `expected ImmutableField, got ${JSON.stringify(r.err)}`);

  // Upsert seed: literal filter only, no operator subdoc.
  // Empty collection 'up'. Filter has $gt; spec sets y; result must omit x.
  r = engine.update('zoo', 'up', ser({ x: { $gt: 5 } }), ser({ $set: { y: 1 } }), false, true);
  assert.equal(r.matched, 0);
  assert.equal(r.upsertedIds.length, 1);
  const upDocs = findAll('zoo', 'up', ser({})).map((b) => BSON.deserialize(b));
  assert.equal(upDocs.length, 1);
  assert.equal(upDocs[0].y, 1);
  assert.equal(upDocs[0].x, undefined, 'upsert must not seed operator clauses');
  assert.ok(upDocs[0]._id, 'upsert must generate _id when not supplied');

  // Upsert with literal _id in filter: that becomes the new doc's _id.
  r = engine.update('zoo', 'up', ser({ _id: 'fixed' }), ser({ $set: { z: 9 } }), false, true);
  assert.equal(r.upsertedIds.length, 1);
  const wrap = BSON.deserialize(Buffer.from(r.upsertedIds[0]));
  assert.equal(wrap._id, 'fixed');

  // Partial match before a TypeMismatch: matched/modified counts must
  // reflect the slots already updated. The engine's update returns an err
  // result; the wire layer's update.ts is what tallies — verified there.
  // Engine surface check: insert mixed types, multi=true $inc → after the
  // first numeric match, the next non-numeric match should error and the
  // earlier mutation must persist (no rollback for in-memory backend).
  engine.insert('zoo', 'mixed', [
    ser({ _id: 1, n: 1 }),
    ser({ _id: 2, n: 'two' }),  // string n, will trip $inc
    ser({ _id: 3, n: 3 }),
  ]);
  r = engine.update('zoo', 'mixed', ser({}), ser({ $inc: { n: 10 } }), true, false);
  assert.equal(r.err?.code, 14);
  assert.equal(r.matched, 1, 'first numeric match must be tallied before error');
  assert.equal(r.modified, 1, 'first match must have been applied');
  const mixed = findAll('zoo', 'mixed', ser({})).map((b) => BSON.deserialize(b));
  const one = mixed.find((d) => d._id === 1);
  assert.equal(one.n, 11, 'first slot must be updated even though batch errored');
}

// erase semantics + tombstone interaction with cursors.
{
  // Reset by using a fresh collection.
  for (let i = 1; i <= 5; ++i) {
    engine.insert('zoo', 'del', [ser({ _id: i })]);
  }

  // Open a cursor mid-stream BEFORE deleting, with batchSize=2.
  const r1 = engine.find('zoo', 'del', ser({}), 2, EMPTY, 0, 0, EMPTY);
  assert.equal(r1.batch.length, 2);
  assert.notEqual(r1.cursorId, 0n);
  const seenIds = r1.batch.map((b) => BSON.deserialize(b)._id);
  assert.deepEqual(seenIds, [1, 2]);

  // Delete a doc the cursor hasn't yet visited.
  const d = engine.erase('zoo', 'del', ser({ _id: 3 }), true);
  assert.equal(d.deleted, 1);

  // Cursor continues — must NOT crash and must skip the tombstoned doc.
  const r2 = engine.getMore(r1.cursorId, 'zoo', 'del', 100);
  const remainingIds = r2.batch.map((b) => BSON.deserialize(b)._id);
  assert.deepEqual(remainingIds, [4, 5], `expected [4,5], got ${remainingIds}`);
  assert.equal(r2.cursorId, 0n);

  // erase single=false removes ALL matching.
  const d2 = engine.erase('zoo', 'del', ser({}), false);
  assert.equal(d2.deleted, 4);  // 1, 2, 4, 5 (3 was already gone)
  assert.equal(findAll('zoo', 'del', ser({})).length, 0);
}

// ---- Slice F1: dot-path nested filters -------------------------------
{
  engine.insert('zoo', 'nested', [
    ser({ _id: 1, address: { city: 'NYC', zip: '10001' }, tags: ['a', 'b'] }),
    ser({ _id: 2, address: { city: 'SF', zip: '94101' }, tags: ['b', 'c'] }),
    ser({ _id: 3, address: { city: 'NYC', zip: '10002' } }),
    ser({ _id: 4 }),  // missing address entirely
    ser({ _id: 5, address: { city: 'LA', meta: { region: 'west' } } }),
  ]);

  // Two-level path equality.
  const nyc = findAll('zoo', 'nested', ser({ 'address.city': 'NYC' })).map(
    (b) => BSON.deserialize(b),
  );
  assert.equal(nyc.length, 2);
  assert.deepEqual(nyc.map((d) => d._id).sort(), [1, 3]);

  // Three-level path.
  const west = findAll(
    'zoo',
    'nested',
    ser({ 'address.meta.region': 'west' }),
  ).map((b) => BSON.deserialize(b));
  assert.equal(west.length, 1);
  assert.equal(west[0]._id, 5);

  // Path with operator ($ne) — missing path matches just like missing field.
  const notSF = findAll(
    'zoo',
    'nested',
    ser({ 'address.city': { $ne: 'SF' } }),
  ).map((b) => BSON.deserialize(b));
  assert.ok(!notSF.some((d) => d._id === 2), 'SF must be excluded');
  assert.ok(notSF.some((d) => d._id === 4), 'doc with no address must match $ne');

  // Array index path — `tags.0` selects the first element.
  const firstA = findAll('zoo', 'nested', ser({ 'tags.0': 'a' })).map((b) =>
    BSON.deserialize(b),
  );
  assert.equal(firstA.length, 1);
  assert.equal(firstA[0]._id, 1);

  // Path against missing intermediate → no match.
  const missing = findAll(
    'zoo',
    'nested',
    ser({ 'address.country': 'US' }),
  );
  assert.equal(missing.length, 0);

  // $exists on a nested path.
  const hasZip = findAll(
    'zoo',
    'nested',
    ser({ 'address.zip': { $exists: true } }),
  ).map((b) => BSON.deserialize(b));
  // _id 1, 2, 3 all have address.zip; _id 4 has no address; _id 5 has no zip.
  assert.equal(hasZip.length, 3);
  assert.deepEqual(hasZip.map((d) => d._id).sort(), [1, 2, 3]);

  // Range op on a nested numeric path.
  engine.insert('zoo', 'nested', [
    ser({ _id: 6, score: { value: 80 } }),
    ser({ _id: 7, score: { value: 50 } }),
  ]);
  const high = findAll(
    'zoo',
    'nested',
    ser({ 'score.value': { $gte: 75 } }),
  ).map((b) => BSON.deserialize(b));
  assert.equal(high.length, 1);
  assert.equal(high[0]._id, 6);
}

// ---- Slice F2: sort / limit / skip -----------------------------------
{
  // Fresh collection. Insert deliberately out of age order, with two ties
  // on age=30 so we can verify stable-sort tiebreak by insertion order.
  const ages = [
    { _id: 1, name: 'alice', age: 40 },
    { _id: 2, name: 'bob',   age: 25 },
    { _id: 3, name: 'carol', age: 30 },  // tie with carol2 below
    { _id: 4, name: 'dan',   age: 30 },  // ditto
    { _id: 5, name: 'eve',   age: 35 },
  ];
  for (const d of ages) engine.insert('zoo', 'sorted', [ser(d)]);

  const sortBy = (spec) =>
    engine
      .find('zoo', 'sorted', ser({}), HUGE, ser(spec), 0, 0, EMPTY)
      .batch.map((b) => BSON.deserialize(b));

  // Ascending sort by age.
  const asc = sortBy({ age: 1 });
  assert.deepEqual(asc.map((d) => d._id), [2, 3, 4, 5, 1]);

  // Descending sort.
  const desc = sortBy({ age: -1 });
  assert.deepEqual(desc.map((d) => d._id), [1, 5, 3, 4, 2]);

  // Stable tiebreak: carol(_id=3) and dan(_id=4) both age=30 — insertion
  // order preserved (3 before 4).
  const tiebreakIdx = asc.findIndex((d) => d.age === 30);
  assert.equal(asc[tiebreakIdx]._id, 3);
  assert.equal(asc[tiebreakIdx + 1]._id, 4);

  // Multi-key sort: by age asc, then name desc.
  engine.insert('zoo', 'sorted', [ser({ _id: 6, name: 'zach', age: 30 })]);
  const multi = engine
    .find('zoo', 'sorted', ser({}), HUGE, ser({ age: 1, name: -1 }), 0, 0, EMPTY)
    .batch.map((b) => BSON.deserialize(b));
  // age=30 trio: zach > dan > carol by name desc.
  const thirties = multi.filter((d) => d.age === 30).map((d) => d.name);
  assert.deepEqual(thirties, ['zach', 'dan', 'carol']);

  // skip drops the first N.
  const skipped = engine
    .find('zoo', 'sorted', ser({}), HUGE, ser({ age: 1 }), 2, 0, EMPTY)
    .batch.map((b) => BSON.deserialize(b));
  assert.equal(skipped.length, 4);
  assert.deepEqual(skipped.map((d) => d._id), [4, 6, 5, 1]);

  // limit caps the result.
  const lim = engine
    .find('zoo', 'sorted', ser({}), HUGE, ser({ age: 1 }), 0, 3, EMPTY)
    .batch.map((b) => BSON.deserialize(b));
  assert.deepEqual(lim.map((d) => d._id), [2, 3, 4]);

  // skip + limit together.
  const window = engine
    .find('zoo', 'sorted', ser({}), HUGE, ser({ age: 1 }), 2, 2, EMPTY)
    .batch.map((b) => BSON.deserialize(b));
  assert.deepEqual(window.map((d) => d._id), [4, 6]);

  // Sort by missing field — missing values come first ascending.
  engine.insert('zoo', 'sorted', [ser({ _id: 7, name: 'noage' })]);
  const withMissing = engine
    .find('zoo', 'sorted', ser({}), HUGE, ser({ age: 1 }), 0, 0, EMPTY)
    .batch.map((b) => BSON.deserialize(b));
  assert.equal(withMissing[0]._id, 7, 'missing field sorts first ascending');

  // Same descending → missing comes last.
  const withMissingDesc = engine
    .find('zoo', 'sorted', ser({}), HUGE, ser({ age: -1 }), 0, 0, EMPTY)
    .batch.map((b) => BSON.deserialize(b));
  assert.equal(
    withMissingDesc[withMissingDesc.length - 1]._id,
    7,
    'missing field sorts last descending',
  );

  // Dot-path sort key.
  engine.insert('zoo', 'nested', [ser({ _id: 100, address: { city: 'Z' } })]);
  const byCity = engine
    .find('zoo', 'nested', ser({}), HUGE, ser({ 'address.city': 1 }), 0, 0, EMPTY)
    .batch.map((b) => BSON.deserialize(b));
  // Among present cities (LA, NYC, NYC, SF, Z), order should be LA<NYC<NYC<SF<Z.
  // Docs without address sort first.
  const presentOrder = byCity
    .filter((d) => d.address?.city)
    .map((d) => d.address.city);
  assert.deepEqual(presentOrder, ['LA', 'NYC', 'NYC', 'SF', 'Z']);

  // Sort + cursor pagination: opens a cursor over the materialized sorted set.
  const r1 = engine.find('zoo', 'sorted', ser({}), 3, ser({ age: 1 }), 0, 0, EMPTY);
  assert.equal(r1.batch.length, 3);
  assert.notEqual(r1.cursorId, 0n);
  const r2 = engine.getMore(r1.cursorId, 'zoo', 'sorted', 100);
  assert.ok(r2.batch.length >= 1);
  assert.equal(r2.cursorId, 0n);
}

// ---- Slice F3: projection -------------------------------------------
{
  for (let i = 1; i <= 3; ++i) {
    engine.insert('zoo', 'proj', [
      ser({ _id: i, name: `n${i}`, age: i * 10, secret: `s${i}` }),
    ]);
  }
  const projFind = (proj) =>
    engine
      .find('zoo', 'proj', ser({}), HUGE, EMPTY, 0, 0, ser(proj))
      .batch.map((b) => BSON.deserialize(b));

  // Inclusion: only listed fields, plus _id by default.
  const incl = projFind({ name: 1 });
  assert.equal(incl.length, 3);
  for (const d of incl) {
    assert.deepEqual(Object.keys(d).sort(), ['_id', 'name']);
  }

  // Inclusion with _id excluded.
  const inclNoId = projFind({ name: 1, _id: 0 });
  for (const d of inclNoId) {
    assert.deepEqual(Object.keys(d), ['name']);
  }

  // Exclusion: everything except listed (and _id stays).
  const excl = projFind({ secret: 0 });
  for (const d of excl) {
    assert.ok(!('secret' in d), 'secret must be excluded');
    assert.ok('name' in d && 'age' in d && '_id' in d);
  }

  // Exclusion of _id alongside other excludes.
  const exclBoth = projFind({ secret: 0, _id: 0 });
  for (const d of exclBoth) {
    assert.deepEqual(Object.keys(d).sort(), ['age', 'name']);
  }

  // Empty projection → all fields preserved.
  const all = projFind({});
  assert.equal(all.length, 3);
  for (const d of all) {
    assert.deepEqual(Object.keys(d).sort(), ['_id', 'age', 'name', 'secret']);
  }

  // Projection through cursor pagination — every getMore must project too.
  const r1 = engine.find('zoo', 'proj', ser({}), 1, EMPTY, 0, 0, ser({ name: 1 }));
  assert.equal(r1.batch.length, 1);
  assert.notEqual(r1.cursorId, 0n);
  let firstDoc = BSON.deserialize(Buffer.from(r1.batch[0]));
  assert.deepEqual(Object.keys(firstDoc).sort(), ['_id', 'name']);

  const r2 = engine.getMore(r1.cursorId, 'zoo', 'proj', 100);
  for (const b of r2.batch) {
    const d = BSON.deserialize(Buffer.from(b));
    assert.deepEqual(Object.keys(d).sort(), ['_id', 'name'],
      'projection must persist across getMore');
  }

  // Projection + sort + limit composed.
  const composed = engine
    .find('zoo', 'proj', ser({}), HUGE, ser({ age: -1 }), 0, 2, ser({ name: 1, _id: 0 }))
    .batch.map((b) => BSON.deserialize(b));
  assert.equal(composed.length, 2);
  assert.deepEqual(composed.map((d) => d.name), ['n3', 'n2']);
  for (const d of composed) {
    assert.deepEqual(Object.keys(d), ['name']);
  }
}

// ---- Slice F4: index create / drop / list ----------------------------
{
  // Backfill counts live scalar values only; missing fields and arrays are
  // skipped until multikey lands alongside filter parity.
  engine.insert('zoo', 'idx_backfill', [
    ser({ _id: 1, species: 'lion' }),
    ser({ _id: 2, species: 'zebra' }),
    ser({ _id: 3 }),
    ser({ _id: 4, species: ['herd'] }),
  ]);
  let r = engine.createIndex('zoo', 'idx_backfill', 'species_1', 'species');
  assert.equal(r.created, true);
  let info = getIndexInfo('zoo', 'idx_backfill', 'species_1');
  assert.ok(info, 'created index must appear in listIndexes');
  assert.equal(info.fieldPath, 'species');
  assert.equal(info.entries, 2, 'backfill should index only live scalar values');

  // Creating the index first means later inserts should auto-maintain it.
  r = engine.createIndex('zoo', 'idx_insert', 'species_1', 'species');
  assert.equal(r.created, true);
  engine.insert('zoo', 'idx_insert', [
    ser({ _id: 1, species: 'giraffe' }),
    ser({ _id: 2, species: 'hippo' }),
    ser({ _id: 3, species: ['skip-me'] }),
  ]);
  info = getIndexInfo('zoo', 'idx_insert', 'species_1');
  assert.equal(info.entries, 2, 'insert should auto-add scalar keys');

  // Update is erase old + insert new. Present->array must remove the old key,
  // and array->scalar must re-add it through the same path.
  r = engine.createIndex('zoo', 'idx_update', 'species_1', 'species');
  assert.equal(r.created, true);
  engine.insert('zoo', 'idx_update', [ser({ _id: 1, species: 'otter' })]);
  info = getIndexInfo('zoo', 'idx_update', 'species_1');
  assert.equal(info.entries, 1);
  let updateRes = engine.update(
    'zoo',
    'idx_update',
    ser({ _id: 1 }),
    ser({ $set: { species: ['raft'] } }),
    false,
    false,
  );
  assert.equal(updateRes.modified, 1);
  info = getIndexInfo('zoo', 'idx_update', 'species_1');
  assert.equal(info.entries, 0, 'update should remove the old scalar key');
  updateRes = engine.update(
    'zoo',
    'idx_update',
    ser({ _id: 1 }),
    ser({ $set: { species: 'seal' } }),
    false,
    false,
  );
  assert.equal(updateRes.modified, 1);
  info = getIndexInfo('zoo', 'idx_update', 'species_1');
  assert.equal(info.entries, 1, 'update should insert the new scalar key');

  // Erase should remove matching keys from the index.
  r = engine.createIndex('zoo', 'idx_erase', 'species_1', 'species');
  assert.equal(r.created, true);
  engine.insert('zoo', 'idx_erase', [
    ser({ _id: 1, species: 'rhino' }),
    ser({ _id: 2, species: 'rhino' }),
  ]);
  info = getIndexInfo('zoo', 'idx_erase', 'species_1');
  assert.equal(info.entries, 2);
  const eraseRes = engine.erase('zoo', 'idx_erase', ser({ _id: 1 }), true);
  assert.equal(eraseRes.deleted, 1);
  info = getIndexInfo('zoo', 'idx_erase', 'species_1');
  assert.equal(info.entries, 1, 'erase should remove the deleted slot');

  // Dropping the index must remove it from listIndexes entirely.
  r = engine.createIndex('zoo', 'idx_drop', 'species_1', 'species');
  assert.equal(r.created, true);
  engine.insert('zoo', 'idx_drop', [ser({ _id: 1, species: 'lynx' })]);
  assert.equal(engine.listIndexes('zoo', 'idx_drop').length, 1);
  const dropRes = engine.dropIndex('zoo', 'idx_drop', 'species_1');
  assert.equal(dropRes.dropped, true);
  assert.equal(engine.listIndexes('zoo', 'idx_drop').length, 0);
}

// ---- Slice F5: planner equality lookup -------------------------------
{
  const plannerDocs = [
    { _id: 1, kind: 'cat', age: 2, habitat: 'house' },
    { _id: 2, kind: 'dog', age: 5, habitat: 'yard' },
    { _id: 3, kind: 'cat', age: 4, habitat: 'yard' },
    { _id: 4, kind: 'cat', age: 2, habitat: 'barn' },
  ];
  for (const doc of plannerDocs) {
    const bytes = ser(doc);
    engine.insert('zoo', 'f5_scan', [bytes]);
    engine.insert('zoo', 'f5_idx', [bytes]);
  }
  let r = engine.createIndex('zoo', 'f5_idx', 'kind_1', 'kind');
  assert.equal(r.created, true);

  // Single indexed equality clause should return byte-identical results.
  const catsScan = findAll('zoo', 'f5_scan', ser({ kind: 'cat' }));
  const catsIdx = findAll('zoo', 'f5_idx', ser({ kind: 'cat' }));
  assertByteIdentical(catsIdx, catsScan, 'single-clause indexed equality');

  // The indexed path must still apply the full filter to non-indexed clauses.
  const catAgeScan = findAll('zoo', 'f5_scan', ser({ kind: 'cat', age: 2 }));
  const catAgeIdx = findAll('zoo', 'f5_idx', ser({ kind: 'cat', age: 2 }));
  assertByteIdentical(catAgeIdx, catAgeScan, 'indexed clause plus residual filter');

  // Two indexable clauses should fall back to scan and still match byte-for-byte.
  r = engine.createIndex('zoo', 'f5_idx', 'age_1', 'age');
  assert.equal(r.created, true);
  const twoClauseScan = findAll('zoo', 'f5_scan', ser({ kind: 'cat', age: 2 }));
  const twoClauseIdx = findAll('zoo', 'f5_idx', ser({ kind: 'cat', age: 2 }));
  assertByteIdentical(twoClauseIdx, twoClauseScan, 'two indexable clauses fallback');

  // Operator subdocs are not indexable in F5 and must preserve scan parity.
  const opScan = findAll('zoo', 'f5_scan', ser({ kind: { $eq: 'cat' } }));
  const opIdx = findAll('zoo', 'f5_idx', ser({ kind: { $eq: 'cat' } }));
  assertByteIdentical(opIdx, opScan, 'operator subdoc fallback');
}

console.log('OK — native engine: CRUD + cursors + filters + nested + sort + projection + index metadata + equality planner');
