import { BSON } from 'bson';
import { getEngine, isEngineLoaded } from '../engine/bridge.js';

export interface StorageConfig {
  backend: 'memory' | 'canopy';
  root?: string;
}

export interface SavannahDBConfig {
  storage?: StorageConfig;
  url?: string;
  authToken?: string;
  // Set to true to allow connecting to a non-loopback host over plain http://.
  // Default false because the REST server has no transport encryption — any
  // bearer token in the Authorization header would be sent in clear text.
  allowInsecureRemote?: boolean;
}

export interface FindOptions {
  sort?: Record<string, number>;
  skip?: number;
  limit?: number;
  projection?: Record<string, number>;
  batchSize?: number;
}

export interface InsertResult {
  insertedCount: number;
}

export interface UpdateResult {
  matched: number;
  modified: number;
  upsertedIds: any[];
}

export interface DeleteResult {
  deleted: number;
}

export interface IndexInfo {
  name: string;
  /** Single-field path; empty for compound indexes. Use fieldPaths for both. */
  fieldPath: string;
  /** Always populated; N entries in declaration order. */
  fieldPaths: string[];
  entries: number;
}

/** Mongo-style index spec: `{a: 1, b: -1}` (sort direction is ignored — all
 *  fields are ascending today). Compound when more than one key. */
export type IndexSpec = string | string[] | Record<string, 1 | -1 | number>;

function normalizeIndexSpec(spec: IndexSpec): string[] {
  if (typeof spec === 'string') return [spec];
  if (Array.isArray(spec)) return [...spec];
  return Object.keys(spec);
}

const EMPTY_BSON = new Uint8Array([5, 0, 0, 0, 0]);

function toBson(obj?: Record<string, any> | null): Uint8Array {
  if (!obj || Object.keys(obj).length === 0) {
    return EMPTY_BSON;
  }
  return BSON.serialize(obj);
}

function isLoopbackHost(host: string): boolean {
  return (
    host === 'localhost' ||
    host === '127.0.0.1' ||
    host === '::1' ||
    host === '[::1]'
  );
}

export class SavannahDB {
  private mode: 'embedded' | 'client';
  private url?: string;
  private authToken?: string;

  constructor(config: SavannahDBConfig = {}) {
    if (config.url) {
      this.mode = 'client';
      this.url = config.url.replace(/\/$/, '');
      this.authToken = config.authToken;

      const parsed = new URL(this.url);
      const isHttp = parsed.protocol === 'http:';
      const loopback = isLoopbackHost(parsed.hostname);
      if (isHttp && !loopback && !config.allowInsecureRemote) {
        throw new Error(
          `Refusing to connect to ${parsed.host} over plain http://. ` +
            'Use https:// or set allowInsecureRemote: true.',
        );
      }
    } else {
      this.mode = 'embedded';
      if (config.storage) {
        process.env.SAVANNAH_STORAGE_BACKEND = config.storage.backend;
        if (config.storage.root) {
          process.env.SAVANNAH_STORAGE_ROOT = config.storage.root;
        }
      }
    }
  }

  getMode(): 'embedded' | 'client' {
    return this.mode;
  }

  getUrl(): string | undefined {
    return this.url;
  }

  getAuthToken(): string | undefined {
    return this.authToken;
  }

  collection(dbName: string, collName: string): Collection {
    return new Collection(this, dbName, collName);
  }
}

export class Collection {
  constructor(
    private db: SavannahDB,
    private dbName: string,
    private collName: string
  ) {}

  async insertMany(docs: Record<string, any>[]): Promise<InsertResult> {
    if (this.db.getMode() === 'embedded') {
      const buffers = docs.map(d => BSON.serialize(d));
      const res = getEngine().insert(this.dbName, this.collName, buffers);
      if (res.err) {
        throw new Error(`${res.err.name}: ${res.err.message} (code ${res.err.code})`);
      }
      return { insertedCount: res.insertedCount };
    } else {
      const res = await this.request('insert', { documents: docs });
      return { insertedCount: res.insertedCount };
    }
  }

  async insertOne(doc: Record<string, any>): Promise<InsertResult> {
    return this.insertMany([doc]);
  }

  find(filter: Record<string, any> = {}, options: FindOptions = {}): Cursor {
    return new Cursor(this.db, this.dbName, this.collName, filter, options);
  }

  async findOne(filter: Record<string, any> = {}, options: FindOptions = {}): Promise<any | null> {
    const cursor = this.find(filter, { ...options, limit: 1 });
    const docs = await cursor.toArray();
    return docs.length > 0 ? docs[0] : null;
  }

  async updateMany(
    filter: Record<string, any>,
    update: Record<string, any>,
    options: { upsert?: boolean } = {}
  ): Promise<UpdateResult> {
    if (this.db.getMode() === 'embedded') {
      const res = getEngine().update(
        this.dbName,
        this.collName,
        toBson(filter),
        toBson(update),
        true,
        !!options.upsert
      );
      if (res.err) {
        throw new Error(`${res.err.name}: ${res.err.message} (code ${res.err.code})`);
      }
      const upsertedIds = res.upsertedIds.map(buf => BSON.deserialize(buf)._id);
      return {
        matched: res.matched,
        modified: res.modified,
        upsertedIds
      };
    } else {
      const res = await this.request('update', {
        filter,
        update,
        multi: true,
        upsert: !!options.upsert
      });
      return res;
    }
  }

  async updateOne(
    filter: Record<string, any>,
    update: Record<string, any>,
    options: { upsert?: boolean } = {}
  ): Promise<UpdateResult> {
    if (this.db.getMode() === 'embedded') {
      const res = getEngine().update(
        this.dbName,
        this.collName,
        toBson(filter),
        toBson(update),
        false,
        !!options.upsert
      );
      if (res.err) {
        throw new Error(`${res.err.name}: ${res.err.message} (code ${res.err.code})`);
      }
      const upsertedIds = res.upsertedIds.map(buf => BSON.deserialize(buf)._id);
      return {
        matched: res.matched,
        modified: res.modified,
        upsertedIds
      };
    } else {
      const res = await this.request('update', {
        filter,
        update,
        multi: false,
        upsert: !!options.upsert
      });
      return res;
    }
  }

  async deleteMany(filter: Record<string, any>): Promise<DeleteResult> {
    if (this.db.getMode() === 'embedded') {
      const res = getEngine().erase(this.dbName, this.collName, toBson(filter), false);
      if (res.err) {
        throw new Error(`${res.err.name}: ${res.err.message} (code ${res.err.code})`);
      }
      return { deleted: res.deleted };
    } else {
      const res = await this.request('delete', { filter, single: false });
      return { deleted: res.deleted };
    }
  }

  async deleteOne(filter: Record<string, any>): Promise<DeleteResult> {
    if (this.db.getMode() === 'embedded') {
      const res = getEngine().erase(this.dbName, this.collName, toBson(filter), true);
      if (res.err) {
        throw new Error(`${res.err.name}: ${res.err.message} (code ${res.err.code})`);
      }
      return { deleted: res.deleted };
    } else {
      const res = await this.request('delete', { filter, single: true });
      return { deleted: res.deleted };
    }
  }

  aggregate(pipeline: Record<string, any>[], options: { batchSize?: number } = {}): Cursor {
    return new Cursor(this.db, this.dbName, this.collName, undefined, {
      batchSize: options.batchSize
    }, pipeline);
  }

  async createIndex(name: string, spec: IndexSpec): Promise<boolean> {
    const fieldPaths = normalizeIndexSpec(spec);
    if (this.db.getMode() === 'embedded') {
      // Pass the array directly; binding accepts string|string[].
      const res = getEngine().createIndex(
        this.dbName,
        this.collName,
        name,
        fieldPaths.length === 1 ? fieldPaths[0]! : fieldPaths,
      );
      if (res.err) {
        throw new Error(`${res.err.name}: ${res.err.message} (code ${res.err.code})`);
      }
      return res.created;
    } else {
      const res = await this.request('indexes/create', { name, fieldPaths });
      return res.created;
    }
  }

  async dropIndex(name: string): Promise<boolean> {
    if (this.db.getMode() === 'embedded') {
      const res = getEngine().dropIndex(this.dbName, this.collName, name);
      if (res.err) {
        throw new Error(`${res.err.name}: ${res.err.message} (code ${res.err.code})`);
      }
      return res.dropped;
    } else {
      const res = await this.request('indexes/drop', { name });
      return res.dropped;
    }
  }

  async listIndexes(): Promise<IndexInfo[]> {
    if (this.db.getMode() === 'embedded') {
      const res = getEngine().listIndexes(this.dbName, this.collName);
      return res.map(idx => ({
        name: idx.name,
        fieldPath: idx.fieldPath,
        fieldPaths: idx.fieldPaths,
        entries: idx.entries
      }));
    } else {
      const res = await this.request('indexes/list', {});
      return res;
    }
  }

  private async request(action: string, payload: Record<string, any>): Promise<any> {
    const url = `${this.db.getUrl()}/api/v1/${this.dbName}/${this.collName}/${action}`;
    const headers: Record<string, string> = { 'Content-Type': 'application/json' };
    const token = this.db.getAuthToken();
    if (token) headers['Authorization'] = `Bearer ${token}`;
    const response = await fetch(url, {
      method: 'POST',
      headers,
      body: JSON.stringify(payload)
    });
    if (!response.ok) {
      const text = await response.text();
      throw new Error(`HTTP ${response.status}: ${text}`);
    }
    const data = await response.json();
    if (data.error) {
      throw new Error(data.error);
    }
    return data;
  }
}

export class Cursor {
  private filter?: Record<string, any>;
  private sortSpec?: Record<string, number>;
  private skipVal = 0;
  private limitVal = 0;
  private projectionSpec?: Record<string, number>;
  private batchSizeVal = 101;
  private pipeline?: Record<string, any>[];

  private cursorIdBig = 0n;
  private cursorIdStr = '0';
  private buffer: any[] = [];
  private index = 0;
  private started = false;

  constructor(
    private db: SavannahDB,
    private dbName: string,
    private collName: string,
    filter?: Record<string, any>,
    options: FindOptions = {},
    pipeline?: Record<string, any>[]
  ) {
    this.filter = filter;
    this.sortSpec = options.sort;
    this.skipVal = options.skip ?? 0;
    this.limitVal = options.limit ?? 0;
    this.projectionSpec = options.projection;
    this.batchSizeVal = options.batchSize ?? 101;
    this.pipeline = pipeline;
  }

  sort(spec: Record<string, number>): this {
    if (this.started) throw new Error('Cannot sort after cursor has started iterating');
    this.sortSpec = spec;
    return this;
  }

  skip(n: number): this {
    if (this.started) throw new Error('Cannot skip after cursor has started iterating');
    this.skipVal = n;
    return this;
  }

  limit(n: number): this {
    if (this.started) throw new Error('Cannot limit after cursor has started iterating');
    this.limitVal = n;
    return this;
  }

  project(spec: Record<string, number>): this {
    if (this.started) throw new Error('Cannot project after cursor has started iterating');
    this.projectionSpec = spec;
    return this;
  }

  batchSize(size: number): this {
    if (this.started) throw new Error('Cannot set batchSize after cursor has started iterating');
    this.batchSizeVal = size;
    return this;
  }

  async next(): Promise<any | null> {
    if (!this.started) {
      await this.start();
    }

    if (this.index < this.buffer.length) {
      return this.buffer[this.index++];
    }

    const hasMore = this.db.getMode() === 'embedded'
      ? this.cursorIdBig !== 0n
      : this.cursorIdStr !== '0' && this.cursorIdStr !== '';

    if (hasMore) {
      await this.fetchMore();
      if (this.index < this.buffer.length) {
        return this.buffer[this.index++];
      }
    }

    return null;
  }

  async toArray(): Promise<any[]> {
    const results: any[] = [];
    let doc: any;
    while ((doc = await this.next()) !== null) {
      results.push(doc);
    }
    return results;
  }

  private async start(): Promise<void> {
    this.started = true;
    this.buffer = [];
    this.index = 0;

    if (this.db.getMode() === 'embedded') {
      let res: { batch: Uint8Array[]; cursorId: bigint };
      if (this.pipeline) {
        const pipelineBytes = BSON.serialize({ pipeline: this.pipeline });
        res = getEngine().aggregate(this.dbName, this.collName, pipelineBytes, this.batchSizeVal);
      } else {
        res = getEngine().find(
          this.dbName,
          this.collName,
          toBson(this.filter),
          this.batchSizeVal,
          toBson(this.sortSpec),
          this.skipVal,
          this.limitVal,
          toBson(this.projectionSpec)
        );
      }
      this.cursorIdBig = res.cursorId;
      this.cursorIdStr = res.cursorId.toString();
      this.buffer = res.batch.map(buf => BSON.deserialize(buf));
    } else {
      // Client mode
      const payload: Record<string, any> = {};
      let endpoint: string;

      if (this.pipeline) {
        endpoint = 'aggregate';
        payload.pipeline = this.pipeline;
        payload.batchSize = this.batchSizeVal;
      } else {
        endpoint = 'find';
        payload.filter = this.filter;
        payload.sort = this.sortSpec;
        payload.skip = this.skipVal;
        payload.limit = this.limitVal;
        payload.projection = this.projectionSpec;
        payload.batchSize = this.batchSizeVal;
      }

      const url = `${this.db.getUrl()}/api/v1/${this.dbName}/${this.collName}/${endpoint}`;
      const headers: Record<string, string> = { 'Content-Type': 'application/json' };
      const token = this.db.getAuthToken();
      if (token) headers['Authorization'] = `Bearer ${token}`;
      const response = await fetch(url, {
        method: 'POST',
        headers,
        body: JSON.stringify(payload)
      });
      if (!response.ok) {
        const text = await response.text();
        throw new Error(`HTTP ${response.status}: ${text}`);
      }
      const data = await response.json();
      if (data.error) {
        throw new Error(data.error);
      }
      this.cursorIdStr = data.cursorId;
      this.cursorIdBig = BigInt(data.cursorId);
      this.buffer = data.batch;
    }
  }

  private async fetchMore(): Promise<void> {
    this.buffer = [];
    this.index = 0;

    if (this.db.getMode() === 'embedded') {
      const res = getEngine().getMore(this.cursorIdBig, this.dbName, this.collName, this.batchSizeVal);
      this.cursorIdBig = res.cursorId;
      this.cursorIdStr = res.cursorId.toString();
      this.buffer = res.batch.map(buf => BSON.deserialize(buf));
    } else {
      // Client mode
      const url = `${this.db.getUrl()}/api/v1/cursors/${this.cursorIdStr}/get-more`;
      const headers: Record<string, string> = { 'Content-Type': 'application/json' };
      const token = this.db.getAuthToken();
      if (token) headers['Authorization'] = `Bearer ${token}`;
      const response = await fetch(url, {
        method: 'POST',
        headers,
        body: JSON.stringify({
          db: this.dbName,
          coll: this.collName,
          batchSize: this.batchSizeVal
        })
      });
      if (!response.ok) {
        const text = await response.text();
        throw new Error(`HTTP ${response.status}: ${text}`);
      }
      const data = await response.json();
      if (data.error) {
        throw new Error(data.error);
      }
      this.cursorIdStr = data.cursorId;
      this.cursorIdBig = BigInt(data.cursorId);
      this.buffer = data.batch;
    }
  }
}
