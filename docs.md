# SavannahDB Exhaustive API & Command Reference

This document serves as the complete technical reference for the SavannahDB engine, APIs, and query languages. It covers the Node.js SDK, the standalone REST API, query and update operators, and the C++ engine's native aggregation pipeline and expression system.

---

## 1. Node.js SDK Reference

The Node.js SDK is importable from `./server/dist/sdk/index.js` and provides a unified, promise-based API.

### `new SavannahDB(options)`
Instantiates the database connection.
* `options.storage`: Configures direct **Embedded Mode** (in-process N-API).
  * `storage.backend`: `'memory'` (in-memory) or `'canopy'` (persistent WAL/snapshot backend).
  * `storage.root`: `string`. The file path on disk where Canopy database files will be persisted.
* `options.url`: Configures **Client Mode** (HTTP REST client connecting to a standalone server, e.g., `'http://localhost:27018'`).

### `db.collection(dbName, collName)`
Returns a `Collection` instance representing a namespace of the form `dbName.collName`.

---

### `Collection` Operations

#### `await collection.insertMany(documents)`
Inserts an array of documents.
* `documents`: `Record<string, any>[]`.
* Returns: `Promise<{ insertedCount: number }>`

#### `await collection.insertOne(document)`
Inserts a single document.
* Returns: `Promise<{ insertedCount: number }>`

#### `collection.find(filter, options)`
Prepares a find query and returns a chainable `Cursor`.
* `filter`: `Record<string, any>`. Match filter.
* `options.sort`: `Record<string, number>`.
* `options.skip`: `number`.
* `options.limit`: `number`.
* `options.projection`: `Record<string, number>`.
* `options.batchSize`: `number`.
* Returns: `Cursor`

#### `await collection.findOne(filter, options)`
Fetches the first document matching `filter`.
* Returns: `Promise<any | null>`

#### `await collection.updateMany(filter, update, options)`
Modifies all documents matching `filter`.
* `update`: Record with update operators (e.g., `$set`) or a whole replacement document.
* `options.upsert`: `boolean`. If true, inserts a new document if no match is found.
* Returns: `Promise<{ matched: number, modified: number, upsertedIds: any[] }>`

#### `await collection.updateOne(filter, update, options)`
Modifies the first document matching `filter`.
* Returns: `Promise<{ matched: number, modified: number, upsertedIds: any[] }>`

#### `await collection.deleteMany(filter)`
Deletes all documents matching `filter`.
* Returns: `Promise<{ deleted: number }>`

#### `await collection.deleteOne(filter)`
Deletes the first document matching `filter`.
* Returns: `Promise<{ deleted: number }>`

#### `collection.aggregate(pipeline, options)`
Prepares a native aggregation pipeline and returns a chainable `Cursor`.
* `pipeline`: `Record<string, any>[]`. Array of stages.
* `options.batchSize`: `number`.
* Returns: `Cursor`

#### `await collection.createIndex(name, fieldPath)`
Triggers B-Tree index creation and automatic backfilling.
* `name`: `string`. Unique index name.
* `fieldPath`: `string`. Field path to index (supports nested paths like `"address.zip"`).
* Returns: `Promise<boolean>`

#### `await collection.dropIndex(name)`
Drops an index.
* Returns: `Promise<boolean>`

#### `await collection.listIndexes()`
* Returns: `Promise<Array<{ name: string, fieldPath: string, entries: number }>>`

---

### `Cursor` Operations
Cursors represent lazy-loaded stream results and support chainable modifiers before execution:

* **`.sort(spec)`**: Sort by fields (`1` for ascending, `-1` for descending).
* **`.skip(n)`**: Number of documents to skip.
* **`.limit(n)`**: Maximum number of documents to return.
* **`.project(spec)`**: Specify projection (`1` to include, `0` to exclude).
* **`.batchSize(n)`**: Pagination size of batches fetched from the server/engine.
* **`await .next()`**: Fetches and returns the next document (`Promise<any | null>`).
* **`await .toArray()`**: Drains the cursor and returns all matched documents (`Promise<any[]>`).

---

## 2. REST HTTP/JSON Endpoint Reference

When running as a standalone server, SavannahDB accepts pure JSON POST requests. Large `BigInt` cursor IDs are seamlessly serialized as `string` values.

### Collection Operations

#### `POST /api/v1/:db/:collection/insert`
* **Body**: `{"documents": [ { "name": "cheetah", "speed": 75 } ]}`
* **Return**: `{"insertedCount": 1}`

#### `POST /api/v1/:db/:collection/find`
* **Body**:
  ```json
  {
    "filter": { "speed": { "$gt": 50 } },
    "sort": { "speed": -1 },
    "skip": 0,
    "limit": 10,
    "projection": { "name": 1 },
    "batchSize": 101
  }
  ```
* **Return**: `{"batch": [{"_id": 5, "name": "cheetah"}], "cursorId": "184029471927"}`

#### `POST /api/v1/:db/:collection/update`
* **Body**: `{"filter": {"_id": 5}, "update": {"$set": {"speed": 80}}, "multi": false, "upsert": false}`
* **Return**: `{"matched": 1, "modified": 1, "upsertedIds": []}`

#### `POST /api/v1/:db/:collection/delete`
* **Body**: `{"filter": {"speed": {"$lt": 10}}, "single": false}`
* **Return**: `{"deleted": 2}`

#### `POST /api/v1/:db/:collection/aggregate`
* **Body**: `{"pipeline": [{"$match": {"speed": {"$gt": 50}}}]}`
* **Return**: `{"batch": [...], "cursorId": "0"}`

### Index Operations

#### `POST /api/v1/:db/:collection/indexes/create`
* **Body**: `{"name": "speed_idx", "fieldPath": "speed"}`
* **Return**: `{"created": true}`

#### `POST /api/v1/:db/:collection/indexes/drop`
* **Body**: `{"name": "speed_idx"}`
* **Return**: `{"dropped": true}`

#### `POST /api/v1/:db/:collection/indexes/list`
* **Body**: `{}`
* **Return**: `[{"name": "speed_idx", "fieldPath": "speed", "entries": 4}]`

### Cursor Operations

#### `POST /api/v1/cursors/:cursorId/get-more`
* **Body**: `{"db": "zoo", "coll": "animals", "batchSize": 101}`
* **Return**: `{"batch": [...], "cursorId": "0"}` (returns `"0"` when cursor is fully exhausted)

#### `POST /api/v1/cursors/kill`
* **Body**: `{"cursorIds": ["184029471927"]}`
* **Return**: `{"killed": ["184029471927"], "notFound": []}`

---

## 3. MQL-style Query Filter Reference

Used inside `collection.find(filter)` and the `$match` aggregation stage.

### Comparison Operators

* **`$eq`**: Matches values equal to a specified value.
  * `{ "age": { "$eq": 42 } }`
* **`$ne`**: Matches values not equal to a specified value (also matches docs missing the field).
  * `{ "age": { "$ne": 42 } }`
* **`$gt` / `$gte`**: Matches values greater than (or equal to) a specified value.
  * `{ "age": { "$gte": 18 } }`
* **`$lt` / `$lte`**: Matches values less than (or equal to) a specified value.
  * `{ "age": { "$lt": 65 } }`
* **`$in`**: Matches any value specified in an array.
  * `{ "name": { "$in": ["lion", "cheetah"] } }`
* **`$nin`**: Matches none of the values specified in an array.
  * `{ "name": { "$nin": ["goose", "duck"] } }`

### Logical Operators

* **`$and`**: Joins clauses with a logical AND.
  * `{ "$and": [ { "type": "electric" }, { "rating": { "$gt": 4.5 } } ] }`
* **`$or`**: Joins clauses with a logical OR.
  * `{ "$or": [ { "type": "electric" }, { "classic": true } ] }`

### Element Operators

* **`$exists`**: Matches documents that have (or lack) the specified field.
  * `{ "age": { "$exists": true } }`

### Evaluation/Regex Operators

* **`$regex`**: Matches strings using regular expressions. Supports regex literals or string clauses.
  * `{ "name": { "$regex": "^lion", "$options": "i" } }`
  * `{ "name": /^lion/i }`

### Nested Dot-Paths
Query nested document structures and specific array elements.
* **Subdocument matching**: `{ "address.city": "NYC" }` matches `{ address: { city: "NYC", zip: "10001" } }`.
* **Array index matching**: `{ "tags.0": "a" }` matches `{ tags: ["a", "b"] }`.

---

## 4. Update Modifiers Reference

Used inside `collection.updateOne` and `updateMany`.

* **`$set`**: Sets the value of a field. Creates the field if it does not exist.
  * `{ "$set": { "status": "processed", "meta.updated": true } }`
* **`$unset`**: Deletes the specified field from the document.
  * `{ "$unset": { "oldField": "" } }`
* **`$inc`**: Increments the value of a numeric field by a specified delta (creates field if missing). Throws `TypeMismatch` (code 14) if applied to non-numeric types.
  * `{ "$inc": { "visits": 1, "score": 10 } }`
* **Document Replacement**: If the update payload does not contain operator keys (no `$` prefixes), it is treated as a direct document replacement. The existing document is wiped and replaced with the payload (retaining its immutable `_id`).
  * `{ "name": "Porsche 911 (New)", "rating": 5.0 }`

---

## 5. Aggregation Pipeline Stages Reference

Aggregation pipelines consist of sequential stages running natively at C++ speed.

### `$match`
Filters the document stream using query filters (identical to `.find` syntax).
```json
{ "$match": { "height": { "$gt": 2.0 } } }
```

### `$group`
Groups input documents by an `_id` expression and accumulates results.
```json
{
  "$group": {
    "_id": "$category",
    "totalCount": { "$sum": 1 },
    "avgScore": { "$avg": "$score" }
  }
}
```
**Supported Group Accumulators**:
* **`$sum`**: Tallies numeric values. `{ "$sum": "$visits" }` or `{ "$sum": 1 }`.
* **`$avg`**: Computes average of numeric inputs. `{ "$avg": "$height" }`.
* **`$min` / `$max`**: Returns minimum/maximum values seen under the group.
* **`$first` / `$last`**: Returns first/last encountered values seen under the group.
* **`$push`**: Collects all values encountered under the group into a single array.

### `$lookup`
Performs left-outer joins against another collection. Supports two forms:

#### 1. Equality Joins (Simple Form)
```json
{
  "$lookup": {
    "from": "owners",
    "localField": "ownerId",
    "foreignField": "_id",
    "as": "ownerDetails"
  }
}
```

#### 2. Pipeline Joins (Complex Form with Variables)
```json
{
  "$lookup": {
    "from": "transactions",
    "let": { "user_id": "$_id", "min_amount": 100 },
    "pipeline": [
      { "$match": { 
          "$and": [
            { "$expr": { "$eq": ["$userId", "$$user_id"] } },
            { "$expr": { "$gt": ["$amount", "$$min_amount"] } }
          ]
      } }
    ],
    "as": "largeTxns"
  }
}
```

### `$unwind`
Deconstructs an array field, emitting a separate document for each element.
```json
{ "$unwind": "$tags" }
```

### `$sort`
Sorts all documents in the pipeline.
```json
{ "$sort": { "score": -1, "name": 1 } }
```

### `$skip`
Skips the first `N` documents.
```json
{ "$skip": 10 }
```

### `$limit`
Limits the pipeline output to the first `N` documents.
```json
{ "$limit": 5 }
```

### `$project`
Reshapes documents by specifying field inclusions, exclusions, and computed fields.
```json
{
  "$project": {
    "_id": 0,
    "name": 1,
    "totalScore": { "$add": ["$mathScore", "$scienceScore"] }
  }
}
```

### `$set` / `$addFields`
Appends new fields or overrides existing fields in the document stream.
```json
{ "$set": { "isAdult": { "$gte": ["$age", 18] } } }
```

### `$unset`
Removes specified fields from the documents.
```json
{ "$unset": ["meta.secret", "tempKey"] }
```

### `$count`
Returns a single document counting total items in the stream.
```json
{ "$count": "total_active_users" }
```

### `$sortByCount`
Groups documents by a given expression, counts them, and sorts the output in descending order.
```json
{ "$sortByCount": "$category" }
```

### `$replaceRoot` / `$replaceWith`
Replaces the entire root document with a nested subdocument or a newly computed object literal.
```json
{ "$replaceWith": "$address" }
```

---

## 6. Recursive Aggregation Expressions & System Operators

Expressions evaluate recursively, allowing complex document mapping.

### System Variables & Identifiers
* **`"$fieldName"`**: Resolves a field (e.g. `"$name"`). Nested paths are evaluated recursively (e.g. `"$address.city"`).
* **`"$$ROOT"`**: Resolves the full root document.
* **`"$$CURRENT"`**: Resolves the current context document.

### Literal Operators
* **`$literal`**: Returns a value without parsing it.
  * `{ "$literal": "$not_a_field" }` (returns the string `"$not_a_field"` literally).

### Conditional Operators
* **`$ifNull`**: Returns the first non-null value among inputs.
  * `{ "$ifNull": ["$score", 0] }`
* **`$cond`**: Evaluates `[if, then, else]` branch expressions.
  * `{ "$cond": [ { "$gte": ["$age", 18] }, "adult", "minor" ] }`
* **`$switch`**: Evaluates multi-branch expressions.
  * `{ "$switch": { "branches": [ { "case": { "$gt": ["$score", 90] }, "then": "A" } ], "default": "F" } }`

### Arithmetic Operators
Each takes an array of numeric expressions and evaluates recursively:
* **`$add` / `$subtract` / `$multiply` / `$divide` / `$mod` / `$pow`**: Standard arithmetic.
  * `{ "$add": ["$basePrice", "$tax"] }`
* **`$abs` / `$ceil` / `$floor` / `$round` / `$trunc` / `$sqrt` / `$exp` / `$log` / `$ln`**: Standard mathematical functions.
  * `{ "$round": ["$rating", 1] }`

### Comparison Expression Operators
Evaluate parameters and return `true` or `false`:
* **`$eq` / `$ne` / `$gt` / `$gte` / `$lt` / `$lte` / `$in`**:
  * `{ "$gt": ["$age", 18] }`

### Boolean Expression Operators
* **`$and` / `$or` / `$not`**: Logical boolean evaluations.
  * `{ "$and": [ { "$gt": ["$age", 18] }, "$hasLicense" ] }`

### String Expression Operators
* **`$concat`**: Combines strings. `{ "$concat": ["$firstName", " ", "$lastName"] }`.
* **`$toString`**: Converts values to strings. `{ "$toString": "$_id" }`.
* **`$toLower` / `$toUpper`**: Modifies case. `{ "$toLower": "$name" }`.
* **`$trim`**: Removes leading/trailing spaces. `{ "$trim": "$address" }`.
* **`$split`**: Splits a string by delimiter. `{ "$split": ["$tags", ","] }`.
* **`$substr` / `$substrCP` / `$substrBytes` / `$strLenCP` / `$strLenBytes`**: Substring and length calculations.

### Array & Object Expression Operators
* **`$size`**: Returns length of array. `{ "$size": "$pack" }`.
* **`$arrayElemAt`**: Grabs item at index. `{ "$arrayElemAt": ["$pack", 0] }`.
* **`$concatArrays`**: Concatenates multiple arrays.
* **`$slice`**: Grabs a slice of an array.
* **`$range`**: Generates integer sequence. `{ "$range": [0, 10, 2] }` -> `[0, 2, 4, 6, 8]`.
* **`$isArray` / `$first` / `$last` / `$reverseArray`**:
* **`$mergeObjects`**: Combines objects. `{ "$mergeObjects": ["$address", { "country": "US" }] }`.
* **`$objectToArray` / `$arrayToObject`**: Transposes between array and key-value objects.
* **`$getField`**: Grabs a specific field by dynamically computed key.

### Type Conversion Operators
* **`$toInt` / `$toLong` / `$toDouble` / `$toBool`**: Casts values.
* **`$type`**: Returns a string representation of the BSON type of the field.
* **`$isNumber`**: Returns `true` if numeric.
