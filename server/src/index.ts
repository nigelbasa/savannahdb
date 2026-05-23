// Public package entrypoint. The SDK (embedded + REST client modes) is the
// primary API. The REST server is available as a subpath import.

export {
  SavannahDB,
  Collection,
  Cursor,
} from './sdk/index.js';

export type {
  StorageConfig,
  SavannahDBConfig,
  FindOptions,
  InsertResult,
  UpdateResult,
  DeleteResult,
  IndexInfo,
} from './sdk/index.js';
