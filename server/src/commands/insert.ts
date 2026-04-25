import { BSON } from 'bson';
import { getEngine } from '../engine/bridge.js';

// Driver may send documents either as a kind-1 doc sequence (common path) or
// inline inside the kind-0 body under `documents`.
export function handleInsert(
  body: Record<string, unknown>,
  docSequences: Map<string, Buffer[]>,
  dbName: string,
): Buffer {
  const coll = String(body.insert);
  let docs: Buffer[] = docSequences.get('documents') ?? [];
  if (docs.length === 0 && Array.isArray(body.documents)) {
    docs = (body.documents as unknown[]).map((d) =>
      Buffer.from(BSON.serialize(d as Record<string, unknown>)),
    );
  }
  const res = getEngine().insert(dbName, coll, docs);
  return Buffer.from(BSON.serialize({ n: res.insertedCount, ok: 1 }));
}
