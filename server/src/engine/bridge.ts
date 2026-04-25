// Typed wrapper around the native N-API addon.

export interface InsertResult {
  insertedCount: number;
}

// cursorId === 0n means the iterator is exhausted. Anything else is a live
// cursor that must be drained via getMore() or released via killCursors().
export interface FindResult {
  batch: Uint8Array[];
  cursorId: bigint;
}

export interface KillCursorsResult {
  killed: bigint[];
  notFound: bigint[];
}

export interface UpdateError {
  code: number;
  name: string;
  message: string;
}

export interface UpdateResult {
  matched: number;
  modified: number;
  // BSON envelopes of the form `{_id: <value>}` for upserts that landed.
  upsertedIds: Uint8Array[];
  err?: UpdateError;
}

export interface EraseResult {
  deleted: number;
}

export interface EngineBindings {
  insert(db: string, coll: string, docs: Uint8Array[]): InsertResult;
  // Pass empty 5-byte BSON docs for `sort` / `projection` to skip them.
  // skip/limit of 0 mean "no skip / no limit". Sort or non-zero skip/limit
  // materializes the result set (streaming preserved only for pure-filter
  // queries). Projection wraps whatever iterator results and applies on
  // every getMore — preserves cursor semantics either way.
  find(
    db: string,
    coll: string,
    filter: Uint8Array,
    batchSize: number,
    sort: Uint8Array,
    skip: number,
    limit: number,
    projection: Uint8Array,
  ): FindResult;
  getMore(
    cursorId: bigint,
    db: string,
    coll: string,
    batchSize: number,
  ): FindResult;
  killCursors(ids: bigint[]): KillCursorsResult;
  update(
    db: string,
    coll: string,
    filter: Uint8Array,
    spec: Uint8Array,
    multi: boolean,
    upsert: boolean,
  ): UpdateResult;
  erase(
    db: string,
    coll: string,
    filter: Uint8Array,
    single: boolean,
  ): EraseResult;
}

let engine: EngineBindings | null = null;

try {
  const { createRequire } = await import('node:module');
  const require = createRequire(import.meta.url);
  engine = require('../../../build/Release/savannah_engine.node') as EngineBindings;
} catch {
  engine = null;
}

export function getEngine(): EngineBindings {
  if (!engine) {
    throw new Error('native engine not loaded — build the C++ addon first');
  }
  return engine;
}

export function isEngineLoaded(): boolean {
  return engine !== null;
}
