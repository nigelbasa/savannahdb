// Typed wrapper around the native N-API addon.

export interface EngineError {
  code: number;
  name: string;
  message: string;
}

export interface InsertResult {
  insertedCount: number;
  err?: EngineError;
}

export interface CreateIndexResult {
  created: boolean;
  err?: EngineError;
}

export interface DropIndexResult {
  dropped: boolean;
  err?: EngineError;
}

export interface IndexInfo {
  name: string;
  // Single-field path. Empty string for compound indexes — use `fieldPaths`.
  fieldPath: string;
  // Always populated. One element for single-field, N for compound, in
  // declaration order.
  fieldPaths: string[];
  entries: number;
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

export interface UpdateResult {
  matched: number;
  modified: number;
  // BSON envelopes of the form `{_id: <value>}` for upserts that landed.
  upsertedIds: Uint8Array[];
  err?: EngineError;
}

export interface EraseResult {
  deleted: number;
  err?: EngineError;
}

export interface EngineBindings {
  insert(db: string, coll: string, docs: Uint8Array[]): InsertResult;
  createIndex(
    db: string,
    coll: string,
    name: string,
    fieldPath: string | string[],
  ): CreateIndexResult;
  dropIndex(db: string, coll: string, name: string): DropIndexResult;
  listIndexes(db: string, coll: string): IndexInfo[];
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
  aggregate(
    db: string,
    coll: string,
    pipeline: Uint8Array,
    batchSize: number,
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
  // node-gyp-build resolves prebuilds/<platform>-<arch>/node.napi.node when
  // installed from npm, or falls back to build/Release/<addon>.node when
  // running from a checkout where cmake-js has just built locally.
  const nodeGypBuild = require('node-gyp-build') as (dir: string) => EngineBindings;
  const { fileURLToPath } = await import('node:url');
  const path = await import('node:path');
  // Walk up from server/dist/engine to the package root (server/) — that's
  // where prebuilds/ lives in the published tarball. For repo-checkout dev
  // the same walk hits the workspace root because build/Release sits there.
  const here = path.dirname(fileURLToPath(import.meta.url));
  const packageRoot = path.resolve(here, '..', '..');
  try {
    engine = nodeGypBuild(packageRoot) as EngineBindings;
  } catch {
    // Repo-checkout fallback: cmake-js builds into <repo-root>/build/Release.
    engine = nodeGypBuild(path.resolve(packageRoot, '..')) as EngineBindings;
  }
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
