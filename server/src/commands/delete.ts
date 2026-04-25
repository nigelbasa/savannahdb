import { BSON } from 'bson';
import { getEngine } from '../engine/bridge.js';

interface DeleteOp {
  q?: Record<string, unknown>;
  limit?: number;  // 0 = all matching, 1 = first match only.
}

// Mongo's `delete` command: ops live either inline under `body.deletes` or
// as a kind-1 doc sequence with identifier "deletes".
export function handleDelete(
  body: Record<string, unknown>,
  docSequences: Map<string, Buffer[]>,
  dbName: string,
): Buffer {
  const coll = String(body.delete);
  const ops: DeleteOp[] = readOps(body, docSequences, 'deletes');

  let n = 0;
  const engine = getEngine();
  for (const op of ops) {
    const filter = Buffer.from(BSON.serialize(op.q ?? {}));
    const single = op.limit === 1;
    const res = engine.erase(dbName, coll, filter, single);
    n += res.deleted;
  }

  return Buffer.from(BSON.serialize({ n, ok: 1 }));
}

function readOps<T>(
  body: Record<string, unknown>,
  docSequences: Map<string, Buffer[]>,
  identifier: string,
): T[] {
  const seq = docSequences.get(identifier);
  if (seq && seq.length > 0) {
    return seq.map((b) => BSON.deserialize(b) as T);
  }
  const inline = body[identifier];
  return Array.isArray(inline) ? (inline as T[]) : [];
}
