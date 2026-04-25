import { BSON, Long } from 'bson';
import { getEngine } from '../engine/bridge.js';

export function handleKillCursors(body: Record<string, unknown>): Buffer {
  const ids = Array.isArray(body.cursors) ? body.cursors : [];
  const bigints: bigint[] = ids.map((id) =>
    id instanceof Long
      ? id.toBigInt()
      : typeof id === 'bigint'
        ? id
        : BigInt(Number(id)),
  );

  const { killed, notFound } = getEngine().killCursors(bigints);

  return Buffer.from(
    BSON.serialize({
      cursorsKilled: killed.map((b) => Long.fromBigInt(b)),
      cursorsNotFound: notFound.map((b) => Long.fromBigInt(b)),
      cursorsAlive: [],
      cursorsUnknown: [],
      ok: 1,
    }),
  );
}
