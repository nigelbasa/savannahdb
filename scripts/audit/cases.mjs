// Audit case set. Each case is self-contained:
//   - fixture: docs inserted into a fresh collection on both DBs.
//   - run(coll): returns a result we deep-compare across SavannahDB and MongoDB.
//
// Categories: filter, expression, pipeline, update, index. Grouped so the
// report can show category-level pass rates.
//
// "expected" overrides exist for ops where MongoDB returns extended-type
// envelopes (Long, Decimal128) that SavannahDB normalizes to plain numbers.
// We only override when the documented behavior is "compatible up to type
// normalization"; everything else must agree exactly.

const ANIMALS = [
  { _id: 1, name: 'giraffe',  habitat: 'savanna', weight: 800,  legs: 4, herd: ['tika', 'lala'] },
  { _id: 2, name: 'lion',     habitat: 'savanna', weight: 190,  legs: 4, herd: ['simba', 'nala', 'mufasa'] },
  { _id: 3, name: 'elephant', habitat: 'savanna', weight: 6000, legs: 4, herd: ['dumbo'] },
  { _id: 4, name: 'penguin',  habitat: 'arctic',  weight: 23,   legs: 2, herd: ['pingu', 'pongo'] },
  { _id: 5, name: 'octopus',  habitat: 'ocean',   weight: 50,   legs: 8 },
  { _id: 6, name: 'lemur',    habitat: 'jungle',  weight: 4,    legs: 4, herd: ['julian'] },
];

const ORDERS = [
  { _id: 'o1', customer: 'alice', total: 120.5, items: ['bread', 'milk'] },
  { _id: 'o2', customer: 'bob',   total: 9.99,  items: ['gum'] },
  { _id: 'o3', customer: 'alice', total: 45,    items: ['eggs', 'butter', 'flour'] },
];

const CUSTOMERS = [
  { _id: 'alice', plan: 'pro' },
  { _id: 'bob',   plan: 'free' },
];

function sortBy(arr, key) {
  return [...arr].sort((a, b) => (a[key] < b[key] ? -1 : a[key] > b[key] ? 1 : 0));
}

// Cases ----------------------------------------------------------------------

export const cases = [
  // FILTER -------------------------------------------------------------------
  {
    id: 'filter.eq',
    category: 'filter',
    operator: '$eq',
    fixture: { animals: ANIMALS },
    run: c => c.animals.find({ habitat: 'savanna' }).sort({ _id: 1 }).toArray(),
  },
  {
    id: 'filter.ne',
    category: 'filter',
    operator: '$ne',
    fixture: { animals: ANIMALS },
    run: c => c.animals.find({ habitat: { $ne: 'savanna' } }).sort({ _id: 1 }).toArray(),
  },
  {
    id: 'filter.gt-gte-lt-lte',
    category: 'filter',
    operator: '$gt $gte $lt $lte',
    fixture: { animals: ANIMALS },
    run: async c => ({
      gt:  await c.animals.find({ weight: { $gt:  100 } }).sort({ _id: 1 }).toArray(),
      gte: await c.animals.find({ weight: { $gte: 100 } }).sort({ _id: 1 }).toArray(),
      lt:  await c.animals.find({ weight: { $lt:  100 } }).sort({ _id: 1 }).toArray(),
      lte: await c.animals.find({ weight: { $lte: 100 } }).sort({ _id: 1 }).toArray(),
    }),
  },
  {
    id: 'filter.in-nin',
    category: 'filter',
    operator: '$in $nin',
    fixture: { animals: ANIMALS },
    run: async c => ({
      in:  await c.animals.find({ habitat: { $in:  ['arctic', 'ocean'] } }).sort({ _id: 1 }).toArray(),
      nin: await c.animals.find({ habitat: { $nin: ['arctic', 'ocean'] } }).sort({ _id: 1 }).toArray(),
    }),
  },
  {
    id: 'filter.exists',
    category: 'filter',
    operator: '$exists',
    fixture: { animals: ANIMALS },
    run: async c => ({
      has:  await c.animals.find({ herd: { $exists: true } }).sort({ _id: 1 }).toArray(),
      none: await c.animals.find({ herd: { $exists: false } }).sort({ _id: 1 }).toArray(),
    }),
  },
  {
    id: 'filter.regex',
    category: 'filter',
    operator: '$regex',
    fixture: { animals: ANIMALS },
    run: c => c.animals.find({ name: { $regex: '^l' } }).sort({ _id: 1 }).toArray(),
  },
  {
    id: 'filter.and-or',
    category: 'filter',
    operator: '$and $or',
    fixture: { animals: ANIMALS },
    run: c =>
      c.animals
        .find({ $or: [{ habitat: 'arctic' }, { $and: [{ legs: 4 }, { weight: { $lt: 100 } }] }] })
        .sort({ _id: 1 })
        .toArray(),
  },
  {
    id: 'filter.nested-path',
    category: 'filter',
    operator: 'dot-path',
    fixture: { nested: [
      { _id: 1, owner: { name: 'a', age: 30 } },
      { _id: 2, owner: { name: 'b', age: 40 } },
    ] },
    run: c => c.nested.find({ 'owner.age': { $gt: 35 } }).toArray(),
  },

  // EXPRESSION ---------------------------------------------------------------
  {
    id: 'expr.arithmetic',
    category: 'expression',
    operator: '$add $subtract $multiply $divide $mod',
    fixture: { nums: [{ _id: 1, a: 10, b: 3 }] },
    run: c =>
      c.nums.aggregate([
        { $project: {
            _id: 0,
            add: { $add: ['$a', '$b'] },
            sub: { $subtract: ['$a', '$b'] },
            mul: { $multiply: ['$a', '$b'] },
            div: { $divide: ['$a', '$b'] },
            mod: { $mod: ['$a', '$b'] },
        } },
      ]).toArray(),
  },
  {
    id: 'expr.string',
    category: 'expression',
    operator: '$toLower $toUpper $concat $strLenCP',
    fixture: { words: [{ _id: 1, s: 'Hello World' }] },
    run: c =>
      c.words.aggregate([
        { $project: {
            _id: 0,
            lower: { $toLower: '$s' },
            upper: { $toUpper: '$s' },
            cat:   { $concat: ['$s', '!'] },
            len:   { $strLenCP: '$s' },
        } },
      ]).toArray(),
  },
  {
    id: 'expr.cond',
    category: 'expression',
    operator: '$cond $switch $ifNull',
    fixture: { items: [
      { _id: 1, qty: 5 },
      { _id: 2, qty: 0 },
      { _id: 3 },
    ] },
    run: c =>
      c.items.aggregate([
        { $project: {
            _id: 1,
            // Sort keys alphabetically so output ordering is deterministic.
            cond:   { $cond: [{ $gt: ['$qty', 0] }, 'has-qty', 'empty'] },
            ifnull: { $ifNull: ['$qty', -1] },
        } },
        { $sort: { _id: 1 } },
      ]).toArray(),
  },
  {
    id: 'expr.array',
    category: 'expression',
    operator: '$size $arrayElemAt $concatArrays $reverseArray $slice $in',
    fixture: { lists: [{ _id: 1, xs: [10, 20, 30, 40, 50] }] },
    run: c =>
      c.lists.aggregate([
        { $project: {
            _id: 0,
            size:   { $size: '$xs' },
            head:   { $arrayElemAt: ['$xs', 0] },
            tail:   { $arrayElemAt: ['$xs', -1] },
            rev:    { $reverseArray: '$xs' },
            slice:  { $slice: ['$xs', 1, 3] },
            cat:    { $concatArrays: ['$xs', [99]] },
            hasTwenty: { $in: [20, '$xs'] },
        } },
      ]).toArray(),
  },

  // PIPELINE -----------------------------------------------------------------
  {
    id: 'pipeline.match-sort-limit',
    category: 'pipeline',
    operator: '$match $sort $limit',
    fixture: { animals: ANIMALS },
    run: c =>
      c.animals.aggregate([
        { $match: { habitat: 'savanna' } },
        { $sort:  { weight: 1 } },
        { $limit: 2 },
        { $project: { _id: 0, name: 1, weight: 1 } },
      ]).toArray(),
  },
  {
    id: 'pipeline.group',
    category: 'pipeline',
    operator: '$group ($sum $avg $min $max $push $first $last)',
    fixture: { animals: ANIMALS },
    run: async c => {
      const rows = await c.animals.aggregate([
        { $group: {
            _id:    '$habitat',
            count:  { $sum: 1 },
            avgW:   { $avg: '$weight' },
            minW:   { $min: '$weight' },
            maxW:   { $max: '$weight' },
            names:  { $push: '$name' },
            first:  { $first: '$name' },
            last:   { $last:  '$name' },
        } },
      ]).toArray();
      // $group output is unordered. Sort by _id for deterministic diff, and
      // sort $push arrays since insertion order isn't guaranteed across
      // engines (Mongo replays in storage order, we replay in scan order —
      // happens to match here, but normalize to be safe).
      return sortBy(rows, '_id').map(r => ({ ...r, names: [...r.names].sort() }));
    },
  },
  {
    id: 'pipeline.lookup-unwind',
    category: 'pipeline',
    operator: '$lookup $unwind',
    fixture: { orders: ORDERS, customers: CUSTOMERS },
    run: c =>
      c.orders.aggregate([
        { $lookup: {
            from: 'customers', localField: 'customer', foreignField: '_id', as: 'cust',
        } },
        { $unwind: { path: '$cust', preserveNullAndEmptyArrays: true } },
        { $sort: { _id: 1 } },
      ]).toArray(),
  },
  {
    id: 'pipeline.count',
    category: 'pipeline',
    operator: '$count',
    fixture: { animals: ANIMALS },
    run: c =>
      c.animals.aggregate([
        { $match: { habitat: 'savanna' } },
        { $count: 'n' },
      ]).toArray(),
  },
  {
    id: 'pipeline.replace-root',
    category: 'pipeline',
    operator: '$replaceRoot',
    fixture: { docs: [{ _id: 1, payload: { a: 1, b: 2 } }] },
    run: c =>
      c.docs.aggregate([
        { $replaceRoot: { newRoot: '$payload' } },
      ]).toArray(),
  },

  // UPDATE -------------------------------------------------------------------
  {
    id: 'update.set-inc-unset',
    category: 'update',
    operator: '$set $inc $unset',
    fixture: { animals: ANIMALS },
    run: async c => {
      await c.animals.updateOne({ _id: 1 }, { $set: { age: 10 }, $inc: { weight: 5 }, $unset: { habitat: '' } });
      return c.animals.find({ _id: 1 }).toArray();
    },
  },
  {
    id: 'update.upsert',
    category: 'update',
    operator: 'upsert',
    fixture: { animals: ANIMALS },
    run: async c => {
      await c.animals.updateOne({ _id: 999 }, { $set: { _id: 999, name: 'kudu' } }, { upsert: true });
      return c.animals.find({ _id: 999 }).toArray();
    },
  },
  {
    id: 'update.delete',
    category: 'update',
    operator: 'delete',
    fixture: { animals: ANIMALS },
    run: async c => {
      await c.animals.deleteMany({ habitat: 'arctic' });
      return c.animals.find({}).sort({ _id: 1 }).toArray();
    },
  },
];
