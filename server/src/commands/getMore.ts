import { BSON, Long } from 'bson';
import { getEngine } from '../engine/bridge.js';

// Mongo's default getMore batch is unbounded; we cap it to a reasonable
// chunk so a single getMore can't pull a huge collection in one shot.
const DEFAULT_GETMORE_BATCH = 1000;

export function handleGetMore(
  body: Record<string, unknown>,
  dbName: string,
): Buffer {
  const cursorIdField = body.getMore;
  const coll = String(body.collection);
  const batchSize =
    typeof body.batchSize === 'number' && body.batchSize > 0
      ? Math.floor(body.batchSize)
      : DEFAULT_GETMORE_BATCH;

  // body.getMore arrives as a Long from the BSON parser.
  const cursorId =
    cursorIdField instanceof Long
      ? cursorIdField.toBigInt()
      : typeof cursorIdField === 'bigint'
        ? cursorIdField
        : BigInt(Number(cursorIdField));

  try {
    const res = getEngine().getMore(cursorId, dbName, coll, batchSize);
    const nextBatch = res.batch.map((b) => BSON.deserialize(Buffer.from(b)));
    return Buffer.from(
      BSON.serialize({
        cursor: {
          id: Long.fromBigInt(res.cursorId),
          ns: `${dbName}.${coll}`,
          nextBatch,
        },
        ok: 1,
      }),
    );
  } catch (err) {
    // Native binding throws "CursorNotFound" for unknown id or ns mismatch.
    // Map to MongoDB error code 43.
    const message = err instanceof Error ? err.message : String(err);
    return Buffer.from(
      BSON.serialize({
        ok: 0,
        errmsg: message,
        code: 43,
        codeName: 'CursorNotFound',
      }),
    );
  }
}
