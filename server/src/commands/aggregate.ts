import { BSON, Long } from 'bson';
import { getEngine } from '../engine/bridge.js';

const DEFAULT_BATCH = 101;

export function handleAggregate(
  body: Record<string, unknown>,
  dbName: string,
): Buffer {
  if (!Array.isArray(body.pipeline)) {
    return commandError('aggregate requires a pipeline array');
  }

  const coll = String(body.aggregate);
  const pipelineBytes = Buffer.from(BSON.serialize({ pipeline: body.pipeline }));
  const cursor =
    body.cursor && typeof body.cursor === 'object' && !Array.isArray(body.cursor)
      ? (body.cursor as Record<string, unknown>)
      : {};
  const batchSize =
    typeof cursor.batchSize === 'number' && cursor.batchSize > 0
      ? Math.floor(cursor.batchSize)
      : DEFAULT_BATCH;

  try {
    const { batch, cursorId } = getEngine().aggregate(
      dbName,
      coll,
      pipelineBytes,
      batchSize,
    );
    return Buffer.from(
      BSON.serialize({
        cursor: {
          id: Long.fromBigInt(cursorId),
          ns: `${dbName}.${coll}`,
          firstBatch: batch.map((doc) => BSON.deserialize(Buffer.from(doc))),
        },
        ok: 1,
      }),
    );
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err);
    return commandError(message);
  }
}

function commandError(errmsg: string): Buffer {
  return Buffer.from(
    BSON.serialize({
      ok: 0,
      errmsg,
      code: 9,
      codeName: 'FailedToParse',
    }),
  );
}
