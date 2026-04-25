import { BSON } from 'bson';
import { getEngine } from '../engine/bridge.js';

interface UpdateOp {
  q?: Record<string, unknown>;
  u?: Record<string, unknown>;
  multi?: boolean;
  upsert?: boolean;
}

// Mongo's `update` command: ops live either inline under `body.updates` or
// as a kind-1 doc sequence with identifier "updates" (the driver's preferred
// path on the wire, but inline is legal too).
export function handleUpdate(
  body: Record<string, unknown>,
  docSequences: Map<string, Buffer[]>,
  dbName: string,
): Buffer {
  const coll = String(body.update);
  const ops: UpdateOp[] = readOps(body, docSequences, 'updates');

  let n = 0;
  let nModified = 0;
  const upserted: { index: number; _id: unknown }[] = [];
  const writeErrors: { index: number; code: number; errmsg: string }[] = [];

  const engine = getEngine();
  for (let i = 0; i < ops.length; ++i) {
    const op = ops[i] as UpdateOp;
    const filter = Buffer.from(BSON.serialize(op.q ?? {}));
    const spec = Buffer.from(BSON.serialize(op.u ?? {}));
    const res = engine.update(
      dbName,
      coll,
      filter,
      spec,
      Boolean(op.multi),
      Boolean(op.upsert),
    );
    // Tally first — `multi:true` may have partial matches before an error
    // (e.g. $inc on a non-numeric field after several numeric ones); Mongo
    // reports both the partial counts AND the writeError.
    n += res.matched + res.upsertedIds.length;
    nModified += res.modified;
    for (const idEnvelope of res.upsertedIds) {
      const wrap = BSON.deserialize(Buffer.from(idEnvelope)) as { _id: unknown };
      upserted.push({ index: i, _id: wrap._id });
    }
    if (res.err) {
      writeErrors.push({ index: i, code: res.err.code, errmsg: res.err.message });
      break;  // ordered semantics — stop on first error.
    }
  }

  const reply: Record<string, unknown> = { n, nModified, ok: 1 };
  if (upserted.length > 0) reply.upserted = upserted;
  if (writeErrors.length > 0) reply.writeErrors = writeErrors;
  return Buffer.from(BSON.serialize(reply));
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
