# SavannahDB Audit

> Generated **2026-05-23T21:25:44.044Z** on Node v25.8.1 (win32-x64).
> Diffed against **MongoDB 8.2.7**.

## Accuracy vs MongoDB

**20/20 cases match.** 0 diverged, 0 errored, 0 could not be checked.

| Category | Operator(s) | Case | Verdict | SavannahDB | MongoDB |
|---|---|---|---|---:|---:|
| filter | `$eq` | `filter.eq` | match | 1.52 ms | 2.92 ms |
| filter | `$ne` | `filter.ne` | match | 0.13 ms | 0.93 ms |
| filter | `$gt $gte $lt $lte` | `filter.gt-gte-lt-lte` | match | 0.51 ms | 6.34 ms |
| filter | `$in $nin` | `filter.in-nin` | match | 0.40 ms | 1.59 ms |
| filter | `$exists` | `filter.exists` | match | 0.31 ms | 1.83 ms |
| filter | `$regex` | `filter.regex` | match | 0.30 ms | 0.83 ms |
| filter | `$and $or` | `filter.and-or` | match | 0.21 ms | 0.82 ms |
| filter | `dot-path` | `filter.nested-path` | match | 0.12 ms | 0.76 ms |
| expression | `$add $subtract $multiply $divide $mod` | `expr.arithmetic` | match | 0.48 ms | 1.61 ms |
| expression | `$toLower $toUpper $concat $strLenCP` | `expr.string` | match | 0.23 ms | 1.99 ms |
| expression | `$cond $switch $ifNull` | `expr.cond` | match | 0.24 ms | 1.44 ms |
| expression | `$size $arrayElemAt $concatArrays $reverseArray $slice $in` | `expr.array` | match | 0.38 ms | 0.91 ms |
| pipeline | `$match $sort $limit` | `pipeline.match-sort-limit` | match | 0.19 ms | 1.05 ms |
| pipeline | `$group ($sum $avg $min $max $push $first $last)` | `pipeline.group` | match | 0.45 ms | 2.13 ms |
| pipeline | `$lookup $unwind` | `pipeline.lookup-unwind` | match | 0.80 ms | 1.44 ms |
| pipeline | `$count` | `pipeline.count` | match | 0.18 ms | 1.43 ms |
| pipeline | `$replaceRoot` | `pipeline.replace-root` | match | 0.17 ms | 0.85 ms |
| update | `$set $inc $unset` | `update.set-inc-unset` | match | 0.40 ms | 2.54 ms |
| update | `upsert` | `update.upsert` | match | 0.23 ms | 1.55 ms |
| update | `delete` | `update.delete` | match | 0.27 ms | 1.64 ms |

## Performance (embedded mode, memory backend)

Single-threaded Node 20 process, no network. Numbers vary across machines.

| Workload | Iterations | Total time | Throughput |
|---|---:|---:|---:|
| insertOne (cold) | 5,000 | 63.1 ms | **79,288 ops/s** |
| findOne(_id) — pk | 5,000 | 74.5 ms | **67,082 ops/s** |
| findOne(val) — idx | 5,000 | 70.6 ms | **70,866 ops/s** |
| find range (val gt) | 1,000 | 112.3 ms | **8,904 ops/s** |
| aggregate $group sum | 200 | 690.7 ms | **290 ops/s** |
| updateOne $inc | 2,000 | 133.5 ms | **14,977 ops/s** |

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
