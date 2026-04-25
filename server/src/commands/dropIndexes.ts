import { BSON } from 'bson';
import { getEngine } from '../engine/bridge.js';

export function handleDropIndexes(
  body: Record<string, unknown>,
  dbName: string,
): Buffer {
  const coll = String(body.dropIndexes);
  const target = body.index;
  const engine = getEngine();
  const indexes = engine.listIndexes(dbName, coll);
  const nIndexesWas = indexes.length;

  if (target === '*') {
    for (const index of indexes) {
      engine.dropIndex(dbName, coll, index.name);
    }
  } else {
    engine.dropIndex(dbName, coll, String(target));
  }

  return Buffer.from(BSON.serialize({ nIndexesWas, ok: 1 }));
}
