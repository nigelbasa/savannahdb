import { BSON, Long } from 'bson';
import { getEngine } from '../engine/bridge.js';

export function handleListIndexes(
  body: Record<string, unknown>,
  dbName: string,
): Buffer {
  const coll = String(body.listIndexes);
  const firstBatch = getEngine().listIndexes(dbName, coll).map((index) => ({
    v: 2,
    key: { [index.fieldPath]: 1 },
    name: index.name,
  }));

  return Buffer.from(
    BSON.serialize({
      cursor: {
        id: Long.fromBigInt(0n),
        ns: `${dbName}.${coll}.$cmd.listIndexes.${coll}`,
        firstBatch,
      },
      ok: 1,
    }),
  );
}
