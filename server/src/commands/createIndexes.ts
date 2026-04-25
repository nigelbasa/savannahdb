import { BSON } from 'bson';
import { getEngine } from '../engine/bridge.js';

interface CreateIndexSpec {
  key?: Record<string, unknown>;
  name?: unknown;
}

export function handleCreateIndexes(
  body: Record<string, unknown>,
  dbName: string,
): Buffer {
  const coll = String(body.createIndexes);
  const specs = Array.isArray(body.indexes) ? (body.indexes as CreateIndexSpec[]) : [];
  const engine = getEngine();
  const numIndexesBefore = engine.listIndexes(dbName, coll).length;

  for (const spec of specs) {
    const parsed = parseIndexSpec(spec);
    if ('error' in parsed) {
      return commandError(parsed.error);
    }
    engine.createIndex(dbName, coll, parsed.name, parsed.fieldPath);
  }

  const numIndexesAfter = engine.listIndexes(dbName, coll).length;
  return Buffer.from(
    BSON.serialize({
      createdCollectionAutomatically: false,
      numIndexesBefore,
      numIndexesAfter,
      ok: 1,
    }),
  );
}

function parseIndexSpec(spec: CreateIndexSpec):
  | { name: string; fieldPath: string }
  | { error: string } {
  if (typeof spec.name !== 'string' || spec.name.length === 0) {
    return { error: 'createIndexes requires each index spec to include a string name' };
  }
  if (!spec.key || typeof spec.key !== 'object' || Array.isArray(spec.key)) {
    return { error: 'createIndexes requires each index spec to include a key document' };
  }
  const entries = Object.entries(spec.key);
  if (entries.length !== 1) {
    return { error: 'compound indexes are not supported yet' };
  }
  const fieldPath = entries[0]?.[0];
  if (typeof fieldPath !== 'string' || fieldPath.length === 0) {
    return { error: 'createIndexes requires a non-empty field path' };
  }
  return { name: spec.name, fieldPath };
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
