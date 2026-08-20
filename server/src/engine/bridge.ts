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

export interface ConfigureResult {
  backend: 'memory' | 'canopy';
  // false when the requested config already matched the live engine.
  changed: boolean;
}

export interface EngineBindings {
  // Select the storage backend explicitly. Must be passed across the native
  // boundary (not via process.env): on Windows, runtime process.env writes go
  // to the Win32 environment block and are invisible to the C runtime's
  // getenv/_dupenv_s that the engine reads, so the canopy choice was silently
  // dropped and the engine fell back to in-memory. A changed root rebuilds the
  // engine, reloading durable state (and dropping any live cursors).
  configure(backend: 'memory' | 'canopy', root?: string): ConfigureResult;
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
// Preserved so getEngine() can report *why* the addon didn't load instead of a
// generic "build it first" — the original code swallowed this and made packaging
// failures (e.g. a bundled binary that can't see prebuilds/) impossible to debug.
let loadError: unknown = null;

try {
  const { createRequire } = await import('node:module');
  const require = createRequire(import.meta.url);
  const { fileURLToPath } = await import('node:url');
  const path = await import('node:path');

  // 1. Explicit override. When the package is embedded in a single-file binary
  //    (bun/pkg/SEA compile), `import.meta.url` points inside the virtual
  //    bundle filesystem, so node-gyp-build's prebuilds/ walk resolves to a
  //    path that doesn't exist on the real disk. The host app ships the .node
  //    alongside the executable and points SAVANNAHDB_ADDON at it. A direct
  //    require() of an absolute path loads fine from the real FS even inside a
  //    compiled binary (verified under bun --compile).
  const override = process.env.SAVANNAHDB_ADDON;
  if (override) {
    engine = require(override) as EngineBindings;
  } else {
    // node-gyp-build resolves prebuilds/<platform>-<arch>/node.napi.node when
    // installed from npm, or falls back to build/Release/<addon>.node when
    // running from a checkout where cmake-js has just built locally.
    const nodeGypBuild = require('node-gyp-build') as (dir: string) => EngineBindings;
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
  }
} catch (e) {
  loadError = e;
  engine = null;
}

export function getEngine(): EngineBindings {
  if (!engine) {
    const detail = loadError
      ? `: ${loadError instanceof Error ? loadError.message : String(loadError)}`
      : ' — build the C++ addon first, or set SAVANNAHDB_ADDON to a prebuilt node.napi.node';
    throw new Error(`native engine not loaded${detail}`);
  }
  return engine;
}

export function isEngineLoaded(): boolean {
  return engine !== null;
}
