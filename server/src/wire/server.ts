import net from 'node:net';
import { BSON } from 'bson';
import {
  encodeOpMsg,
  encodeOpReply,
  tryParseMessage,
  type IncomingFrame,
} from './parser.js';
import { Session } from './session.js';
import { handleHello } from '../commands/hello.js';
import {
  handleBuildInfo,
  handleEndSessions,
  handleGetLog,
  handlePing,
} from '../commands/admin.js';
import { handleInsert } from '../commands/insert.js';
import { handleFind } from '../commands/find.js';
import { handleGetMore } from '../commands/getMore.js';
import { handleKillCursors } from '../commands/killCursors.js';
import { handleUpdate } from '../commands/update.js';
import { handleDelete } from '../commands/delete.js';
import { handleCreateIndexes } from '../commands/createIndexes.js';
import { handleDropIndexes } from '../commands/dropIndexes.js';
import { handleListIndexes } from '../commands/listIndexes.js';

export function createServer(port: number) {
  const server = net.createServer((socket) => {
    const session = new Session();
    console.log(
      `[conn ${session.connectionId}] connected ${socket.remoteAddress}:${socket.remotePort}`,
    );

    let buf = Buffer.alloc(0);

    socket.on('data', (chunk) => {
      buf = buf.length === 0 ? chunk : Buffer.concat([buf, chunk]);
      try {
        while (true) {
          const parsed = tryParseMessage(buf);
          if (!parsed) break;
          buf = buf.subarray(parsed.consumed);
          const reply = handleFrame(session, parsed.frame);
          socket.write(reply);
        }
      } catch (err) {
        console.error(`[conn ${session.connectionId}] parse error:`, err);
        socket.destroy();
      }
    });

    socket.on('close', () => {
      console.log(`[conn ${session.connectionId}] closed`);
    });
    socket.on('error', (err) => {
      console.error(`[conn ${session.connectionId}] socket error:`, err);
    });
  });

  server.listen(port, () => {
    console.log(`SavannahDB listening on ${port}`);
  });

  return server;
}

function handleFrame(session: Session, frame: IncomingFrame): Buffer {
  if (frame.kind === 'OP_QUERY') {
    const query = BSON.deserialize(frame.query) as Record<string, unknown>;
    const cmd = firstKey(query);
    const responseBson = dispatch(cmd, session, query, new Map());
    return encodeOpReply(frame.header.requestId, session.allocRequestId(), responseBson);
  }

  const doc = BSON.deserialize(frame.body) as Record<string, unknown>;
  const cmd = firstKey(doc);
  const seqMap = new Map<string, Buffer[]>();
  for (const s of frame.docSequences) seqMap.set(s.identifier, s.documents);
  const responseBson = dispatch(cmd, session, doc, seqMap);
  return encodeOpMsg(frame.header.requestId, session.allocRequestId(), responseBson);
}

function dispatch(
  cmd: string,
  session: Session,
  doc: Record<string, unknown>,
  seqs: Map<string, Buffer[]>,
): Buffer {
  const dbName = typeof doc.$db === 'string' ? doc.$db : 'admin';
  switch (cmd) {
    case 'hello':
    case 'isMaster':
    case 'ismaster':
      return handleHello(session);
    case 'ping':
      return handlePing();
    case 'buildInfo':
    case 'buildinfo':
      return handleBuildInfo();
    case 'getLog':
      return handleGetLog();
    case 'endSessions':
      return handleEndSessions();
    case 'insert':
      return handleInsert(doc, seqs, dbName);
    case 'createIndexes':
      return handleCreateIndexes(doc, dbName);
    case 'dropIndexes':
      return handleDropIndexes(doc, dbName);
    case 'listIndexes':
      return handleListIndexes(doc, dbName);
    case 'find':
      return handleFind(doc, dbName);
    case 'getMore':
      return handleGetMore(doc, dbName);
    case 'killCursors':
      return handleKillCursors(doc);
    case 'update':
      return handleUpdate(doc, seqs, dbName);
    case 'delete':
      return handleDelete(doc, seqs, dbName);
    default:
      console.log(
        `[conn ${session.connectionId}] UNKNOWN cmd=${cmd} keys=${Object.keys(doc).join(',')}`,
      );
      return Buffer.from(
        BSON.serialize({
          ok: 0,
          errmsg: `command ${cmd} not implemented`,
          code: 59,
          codeName: 'CommandNotFound',
        }),
      );
  }
}

function firstKey(doc: Record<string, unknown>): string {
  for (const k of Object.keys(doc)) return k;
  return '';
}
