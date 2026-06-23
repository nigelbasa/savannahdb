# SavannahDB Audit

> Generated **2026-06-23T17:57:49.913Z** on Node v25.8.1 (win32-x64).
> Diffed against **MongoDB unavailable**.

## Accuracy vs MongoDB

**0/20 cases match.** 0 diverged, 0 errored, 20 could not be checked.

| Category | Operator(s) | Case | Verdict | SavannahDB | MongoDB |
|---|---|---|---|---:|---:|
| filter | `$eq` | `filter.eq` | _skipped_ | 2.10 ms | — |
| filter | `$ne` | `filter.ne` | _skipped_ | 0.14 ms | — |
| filter | `$gt $gte $lt $lte` | `filter.gt-gte-lt-lte` | _skipped_ | 0.44 ms | — |
| filter | `$in $nin` | `filter.in-nin` | _skipped_ | 0.19 ms | — |
| filter | `$exists` | `filter.exists` | _skipped_ | 0.26 ms | — |
| filter | `$regex` | `filter.regex` | _skipped_ | 1.36 ms | — |
| filter | `$and $or` | `filter.and-or` | _skipped_ | 0.18 ms | — |
| filter | `dot-path` | `filter.nested-path` | _skipped_ | 0.11 ms | — |
| expression | `$add $subtract $multiply $divide $mod` | `expr.arithmetic` | _skipped_ | 1.08 ms | — |
| expression | `$toLower $toUpper $concat $strLenCP` | `expr.string` | _skipped_ | 0.15 ms | — |
| expression | `$cond $switch $ifNull` | `expr.cond` | _skipped_ | 0.16 ms | — |
| expression | `$size $arrayElemAt $concatArrays $reverseArray $slice $in` | `expr.array` | _skipped_ | 0.28 ms | — |
| pipeline | `$match $sort $limit` | `pipeline.match-sort-limit` | _skipped_ | 0.18 ms | — |
| pipeline | `$group ($sum $avg $min $max $push $first $last)` | `pipeline.group` | _skipped_ | 0.53 ms | — |
| pipeline | `$lookup $unwind` | `pipeline.lookup-unwind` | _skipped_ | 0.48 ms | — |
| pipeline | `$count` | `pipeline.count` | _skipped_ | 0.10 ms | — |
| pipeline | `$replaceRoot` | `pipeline.replace-root` | _skipped_ | 0.07 ms | — |
| update | `$set $inc $unset` | `update.set-inc-unset` | _skipped_ | 0.45 ms | — |
| update | `upsert` | `update.upsert` | _skipped_ | 0.17 ms | — |
| update | `delete` | `update.delete` | _skipped_ | 0.20 ms | — |

## Performance (embedded mode, memory backend)

Single-threaded Node 20 process, no network. Numbers vary across machines.

| Workload | Iterations | Total time | Throughput |
|---|---:|---:|---:|
| insertOne (cold) | 5,000 | 57.7 ms | **86,668 ops/s** |
| findOne(_id) — pk | 5,000 | 68.8 ms | **72,721 ops/s** |
| findOne(val) — idx | 5,000 | 64.2 ms | **77,854 ops/s** |
| find range (val gt) | 1,000 | 108.3 ms | **9,232 ops/s** |
| aggregate $group sum | 200 | 678.3 ms | **295 ops/s** |
| updateOne $inc | 2,000 | 159.6 ms | **12,529 ops/s** |

## Performance vs SQLite (better-sqlite3)

Both engines run in-process. SQLite uses prepared statements; SavannahDB
uses its public SDK API. **Ratio** = SavannahDB / SQLite — values ≥ 1.0 mean
SavannahDB is at least as fast; < 1.0 means SQLite wins. Rows flagged with
**⚠** are > 2× slower than SQLite — those are the optimisation targets.

| Workload | SavannahDB | SQLite | Ratio |
|---|---:|---:|---:|
| insertOne | 90,823 ops/s | 444,247 ops/s | **0.20** ⚠ |
| findOne(_id) — pk | 62,764 ops/s | 839,490 ops/s | **0.07** ⚠ |
| findOne(val) — idx | 78,302 ops/s | 677,975 ops/s | **0.12** ⚠ |
| updateOne $inc | 15,439 ops/s | 339,645 ops/s | **0.05** ⚠ |
| bulk insert (50,000 docs, _id only) | 199,857 ops/s | 859,437 ops/s | **0.23** ⚠ |
| bulk insert (50,000 docs, +secondary idx) | 121,757 ops/s | 558,355 ops/s | **0.22** ⚠ |
| build secondary index over 50,000 docs | 279.5 ms | 21.1 ms | **0.08** ⚠ |
| range scan (val gt, 50,000 docs) | 5,344 ops/s | 11,500 ops/s | **0.46** ⚠ |
| aggregate $group sum (50,000 docs) | 21 ops/s | 422 ops/s | **0.05** ⚠ |

## Persistence (Canopy backend)

**All checks pass.** WAL + checkpoint durability verified across process restarts.

| Step | Result |
|---|---|
| insert-then-exit | pass |
| reopen-read | pass |
| delete-survives-restart | pass |

## Operator coverage

Status legend: ● fully covered by audit cases · ○ implemented, not exercised in this run · ◐ partial (see notes)

### Filter operators

| Operator | Status |
|---|:-:|
| `$eq $ne $gt $gte $lt $lte` | ● |
| `$in $nin` | ● |
| `$exists` | ● |
| `$regex (+ $options, /pat/i sugar)` | ● |
| `$and $or` | ● |
| `$expr` | ○ |
| `dot-path nested field reads` | ● |

### Update operators

| Operator | Status |
|---|:-:|
| `$set` | ● |
| `$unset` | ● |
| `$inc` | ● |
| `replacement update (no $-keys)` | ○ |
| `upsert` | ● |

### Pipeline stages

| Operator | Status |
|---|:-:|
| `$match` | ● |
| `$sort` | ● |
| `$skip` | ○ |
| `$limit` | ● |
| `$project` | ● |
| `$set / $addFields` | ○ |
| `$unset` | ○ |
| `$group` | ● |
| `$count` | ● |
| `$sortByCount` | ○ |
| `$lookup` | ● |
| `$replaceRoot` | ● |
| `$replaceWith` | ○ |
| `$unwind` | ● |

### $group accumulators

| Operator | Status |
|---|:-:|
| `$sum $avg $min $max` | ● |
| `$first $last` | ● |
| `$push` | ● |

### Expression operators

| Operator | Status |
|---|:-:|
| `$literal` | ○ |
| `$ifNull` | ● |
| `Arithmetic: $add $subtract $multiply $divide $mod` | ● |
| `Math: $abs $ceil $floor $trunc $round $pow $sqrt $exp $ln $log $log10` | ○ |
| `Comparison: $eq $ne $gt $gte $lt $lte` | ○ |
| `Conditional: $cond $switch` | ● |
| `Boolean: $and $or $not` | ○ |
| `String: $toLower $toUpper $trim $substr $split $strLenCP $strLenBytes $concat` | ● |
| `Type conv: $toInt $toLong $toDouble $toBool $type $isNumber $toString` | ○ |
| `Array: $size $arrayElemAt $concatArrays $slice $range $reverseArray $first $last $isArray $in` | ◐ (see notes) |
| `Object: $mergeObjects $objectToArray $arrayToObject $getField` | ○ |
| `Aggregator-as-expr: $sum $avg $min $max (over an array)` | ○ |

## Known limitations

Surfaced by the audit run; these are compatibility notes rather than bugs.

- **N-API number ceiling.** All integer accumulator results are returned as JS `number`. Sums above 2^53 will lose precision. Use `bson.Long` casting at the call site if you need 64-bit integer fidelity.
- **ECMAScript regex flavor.** `$regex` uses `std::regex` ECMAScript mode. PCRE-only features (lookbehind, named groups beyond ES2018) are not supported; `s` (dotall) and `x` (extended) flags are silently ignored.
- **No compound or partial indexes yet.** Single-field indexes only. Multikey (array) indexes, unique constraints, partial / sparse / TTL, and text / geo indexes are all deferred until drivers prove them necessary.
- **Insert throughput trades off against implicit `_id` index.** Maintaining the index on every write costs ~30% versus an index-free insert path. Worth it given the 20× speedup on `findOne({_id})` lookups; raise an issue if your workload is insert-dominant and never reads by `_id`.

## Reproducing this report

```bash
# Run audit against MongoDB on default port (set MONGODB_URI if non-default)
node scripts/audit/run.mjs
# Re-render the markdown from the JSON
node scripts/audit/render.mjs
```

Without MongoDB the audit still runs SavannahDB-only — accuracy verdicts become "skipped" but perf and persistence numbers are still produced.
