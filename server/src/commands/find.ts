import { BSON, Long } from 'bson';
import { getEngine } from '../engine/bridge.js';

// Mongo's wire default for `find` first batch is 101 docs.
//
// Deferred to Phase 0.3+: `projection` (next slice). `sort`/`limit`/`skip`
// are wired here but the engine materializes the full result for sorted
// queries — streaming returns once indexes land.
const DEFAULT_FIND_BATCH = 101;
const EMPTY_BSON = Buffer.from([5, 0, 0, 0, 0]);

export function handleFind(
  body: Record<string, unknown>,
  dbName: string,
): Buffer {
  const coll = String(body.find);
  const filterDoc =
    body.filter && typeof body.filter === 'object'
      ? (body.filter as Record<string, unknown>)
      : {};
  const filterBytes = Buffer.from(BSON.serialize(filterDoc));

  const sortBytes =
    body.sort && typeof body.sort === 'object'
      ? Buffer.from(BSON.serialize(body.sort as Record<string, unknown>))
      : EMPTY_BSON;
  const projectionBytes =
    body.projection && typeof body.projection === 'object'
      ? Buffer.from(BSON.serialize(body.projection as Record<string, unknown>))
      : EMPTY_BSON;

  const batchSize =
    typeof body.batchSize === 'number' && body.batchSize > 0
      ? Math.floor(body.batchSize)
      : DEFAULT_FIND_BATCH;
  const skip = typeof body.skip === 'number' && body.skip > 0 ? Math.floor(body.skip) : 0;
  const limit =
    typeof body.limit === 'number' && body.limit > 0 ? Math.floor(body.limit) : 0;

  const { batch, cursorId } = getEngine().find(
    dbName,
    coll,
    filterBytes,
    batchSize,
    sortBytes,
    skip,
    limit,
    projectionBytes,
  );
  const firstBatch = batch.map((b) => BSON.deserialize(Buffer.from(b)));
  return Buffer.from(
    BSON.serialize({
      cursor: {
        id: Long.fromBigInt(cursorId),
        ns: `${dbName}.${coll}`,
        firstBatch,
      },
      ok: 1,
    }),
  );
}
