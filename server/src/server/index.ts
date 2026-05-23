import http from 'node:http';
import { BSON } from 'bson';
import { getEngine, isEngineLoaded } from '../engine/bridge.js';

const EMPTY_BSON = new Uint8Array([5, 0, 0, 0, 0]);

const DEFAULT_MAX_BODY_BYTES = 16 * 1024 * 1024;
const DEFAULT_REQUEST_TIMEOUT_MS = 30_000;
const DEFAULT_HOST = '127.0.0.1';

function toBson(obj?: Record<string, any> | null): Uint8Array {
  if (!obj || Object.keys(obj).length === 0) {
    return EMPTY_BSON;
  }
  return BSON.serialize(obj);
}

class HttpError extends Error {
  constructor(public status: number, public publicMessage: string) {
    super(publicMessage);
  }
}

function getBody(req: http.IncomingMessage, maxBytes: number): Promise<string> {
  return new Promise((resolve, reject) => {
    const declared = Number(req.headers['content-length']);
    if (Number.isFinite(declared) && declared > maxBytes) {
      reject(new HttpError(413, 'Payload Too Large'));
      return;
    }
    const chunks: Buffer[] = [];
    let received = 0;
    req.on('data', (chunk: Buffer) => {
      received += chunk.length;
      if (received > maxBytes) {
        reject(new HttpError(413, 'Payload Too Large'));
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });
    req.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
    req.on('error', err => reject(err));
  });
}

function sendJson(res: http.ServerResponse, status: number, data: any) {
  res.writeHead(status, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify(data));
}

// Map engine errors (which may include filesystem paths or libbson detail) to
// a stable shape clients can act on without leaking internals.
function publicEngineError(err: { name?: string; code?: number; message?: string }): {
  error: string;
  code?: number;
} {
  const safeName = err.name && /^[A-Za-z0-9_]+$/.test(err.name) ? err.name : 'EngineError';
  return err.code !== undefined
    ? { error: safeName, code: err.code }
    : { error: safeName };
}

function timingSafeEqual(a: string, b: string): boolean {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) {
    diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  }
  return diff === 0;
}

export interface ServerOptions {
  port: number;
  host?: string;
  authToken?: string;
  maxBodyBytes?: number;
  requestTimeoutMs?: number;
}

export function startServer(portOrOptions: number | ServerOptions): http.Server {
  const opts: ServerOptions =
    typeof portOrOptions === 'number' ? { port: portOrOptions } : portOrOptions;

  const host = opts.host ?? process.env.SAVANNAH_HOST ?? DEFAULT_HOST;
  const authToken = opts.authToken ?? process.env.SAVANNAH_AUTH_TOKEN ?? '';
  const maxBodyBytes = opts.maxBodyBytes ?? DEFAULT_MAX_BODY_BYTES;
  const requestTimeoutMs = opts.requestTimeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS;

  const requireAuth = authToken.length > 0;
  if (!requireAuth && host !== '127.0.0.1' && host !== 'localhost' && host !== '::1') {
    // Refuse to bind a no-auth listener to anything other than loopback.
    throw new Error(
      `Refusing to start unauthenticated REST server on non-loopback host "${host}". ` +
        'Set SAVANNAH_AUTH_TOKEN or bind to 127.0.0.1.',
    );
  }

  const server = http.createServer(async (req, res) => {
    try {
      const url = req.url || '';
      const method = req.method || 'GET';

      if (method !== 'POST') {
        return sendJson(res, 405, { error: 'Method Not Allowed' });
      }

      if (requireAuth) {
        const header = req.headers['authorization'];
        const presented =
          typeof header === 'string' && header.startsWith('Bearer ')
            ? header.slice('Bearer '.length)
            : '';
        if (!presented || !timingSafeEqual(presented, authToken)) {
          return sendJson(res, 401, { error: 'Unauthorized' });
        }
      }

      const bodyText = await getBody(req, maxBodyBytes);

      // Routing logic
      // 1. Cursors endpoints
      // POST /api/v1/cursors/:cursorId/get-more
      const getMoreMatch = url.match(/^\/api\/v1\/cursors\/([^\/]+)\/get-more$/);
      if (getMoreMatch) {
        const cursorIdStr = getMoreMatch[1]!;
        const { db, coll, batchSize } = JSON.parse(bodyText);
        const cursorId = BigInt(cursorIdStr);

        const engineRes = getEngine().getMore(cursorId, db, coll, batchSize ?? 101);
        const batch = engineRes.batch.map(buf => BSON.deserialize(Buffer.from(buf)));
        return sendJson(res, 200, {
          batch,
          cursorId: engineRes.cursorId.toString()
        });
      }

      // POST /api/v1/cursors/kill
      if (url === '/api/v1/cursors/kill') {
        const { cursorIds } = JSON.parse(bodyText);
        const ids = (cursorIds || []).map((id: string) => BigInt(id));
        const engineRes = getEngine().killCursors(ids);
        return sendJson(res, 200, {
          killed: engineRes.killed.map(id => id.toString()),
          notFound: engineRes.notFound.map(id => id.toString())
        });
      }

      // 2. Collection endpoints
      // POST /api/v1/:db/:collection/:action
      const collMatch = url.match(/^\/api\/v1\/([^\/]+)\/([^\/]+)\/(.+)$/);
      if (collMatch) {
        const db = collMatch[1]!;
        const coll = collMatch[2]!;
        const action = collMatch[3]!;
        const payload = bodyText ? JSON.parse(bodyText) : {};

        switch (action) {
          case 'insert': {
            const documents = payload.documents || [];
            const buffers = documents.map((doc: any) => BSON.serialize(doc));
            const engineRes = getEngine().insert(db, coll, buffers);
            if (engineRes.err) {
              return sendJson(res, 400, publicEngineError(engineRes.err));
            }
            return sendJson(res, 200, { insertedCount: engineRes.insertedCount });
          }

          case 'find': {
            const engineRes = getEngine().find(
              db,
              coll,
              toBson(payload.filter),
              payload.batchSize ?? 101,
              toBson(payload.sort),
              payload.skip ?? 0,
              payload.limit ?? 0,
              toBson(payload.projection)
            );
            const batch = engineRes.batch.map(buf => BSON.deserialize(Buffer.from(buf)));
            return sendJson(res, 200, {
              batch,
              cursorId: engineRes.cursorId.toString()
            });
          }

          case 'update': {
            const engineRes = getEngine().update(
              db,
              coll,
              toBson(payload.filter),
              toBson(payload.update),
              !!payload.multi,
              !!payload.upsert
            );
            if (engineRes.err) {
              return sendJson(res, 400, publicEngineError(engineRes.err));
            }
            const upsertedIds = engineRes.upsertedIds.map(buf => BSON.deserialize(Buffer.from(buf))._id);
            return sendJson(res, 200, {
              matched: engineRes.matched,
              modified: engineRes.modified,
              upsertedIds
            });
          }

          case 'delete': {
            const engineRes = getEngine().erase(db, coll, toBson(payload.filter), !!payload.single);
            if (engineRes.err) {
              return sendJson(res, 400, publicEngineError(engineRes.err));
            }
            return sendJson(res, 200, { deleted: engineRes.deleted });
          }

          case 'aggregate': {
            const pipelineBytes = BSON.serialize({ pipeline: payload.pipeline || [] });
            const engineRes = getEngine().aggregate(db, coll, pipelineBytes, payload.batchSize ?? 101);
            const batch = engineRes.batch.map(buf => BSON.deserialize(Buffer.from(buf)));
            return sendJson(res, 200, {
              batch,
              cursorId: engineRes.cursorId.toString()
            });
          }

          case 'indexes/create': {
            // Accept either `fieldPath: string` (single) or `fieldPaths: string[]`
            // (compound). The binding accepts string | string[] directly.
            const spec: string | string[] = Array.isArray(payload.fieldPaths)
              ? payload.fieldPaths
              : payload.fieldPath;
            const engineRes = getEngine().createIndex(db, coll, payload.name, spec);
            if (engineRes.err) {
              return sendJson(res, 400, publicEngineError(engineRes.err));
            }
            return sendJson(res, 200, { created: engineRes.created });
          }

          case 'indexes/drop': {
            const engineRes = getEngine().dropIndex(db, coll, payload.name);
            if (engineRes.err) {
              return sendJson(res, 400, publicEngineError(engineRes.err));
            }
            return sendJson(res, 200, { dropped: engineRes.dropped });
          }

          case 'indexes/list': {
            const engineRes = getEngine().listIndexes(db, coll);
            const indexes = engineRes.map(idx => ({
              name: idx.name,
              fieldPath: idx.fieldPath,
              fieldPaths: idx.fieldPaths,
              entries: idx.entries
            }));
            return sendJson(res, 200, indexes);
          }

          default:
            return sendJson(res, 404, { error: 'Unknown action' });
        }
      }

      return sendJson(res, 404, { error: 'Not Found' });
    } catch (err: any) {
      if (err instanceof HttpError) {
        return sendJson(res, err.status, { error: err.publicMessage });
      }
      // Log raw detail server-side; do not leak it to the client.
      console.error('Request error:', err);
      return sendJson(res, 500, { error: 'Internal Server Error' });
    }
  });

  server.requestTimeout = requestTimeoutMs;
  server.headersTimeout = requestTimeoutMs;

  server.listen(opts.port, host, () => {
    const authNote = requireAuth ? 'auth required' : 'no auth (loopback only)';
    console.log(`SavannahDB REST Server listening on ${host}:${opts.port} (${authNote})`);
  });

  return server;
}
