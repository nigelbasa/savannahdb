// MongoDB wire protocol framing.
// OP_MSG (2013) is the modern opcode. OP_QUERY (2004) is still sent by drivers
// for the legacy `isMaster` probe before they decide to speak OP_MSG; we accept
// it and respond with OP_REPLY (1) so wire-version negotiation can complete.

export const OP_REPLY = 1;
export const OP_QUERY = 2004;
export const OP_MSG = 2013;
export const MSG_HEADER_SIZE = 16;

export interface MsgHeader {
  messageLength: number;
  requestId: number;
  responseTo: number;
  opCode: number;
}

export interface DocumentSequence {
  identifier: string;
  documents: Buffer[];
}

export interface OpMsgFrame {
  kind: 'OP_MSG';
  header: MsgHeader;
  flagBits: number;
  body: Buffer;
  docSequences: DocumentSequence[];
}

export interface OpQueryFrame {
  kind: 'OP_QUERY';
  header: MsgHeader;
  flags: number;
  fullCollectionName: string;
  numberToSkip: number;
  numberToReturn: number;
  query: Buffer;
}

export type IncomingFrame = OpMsgFrame | OpQueryFrame;

export interface ParseResult {
  frame: IncomingFrame;
  consumed: number;
}

export function tryParseMessage(buf: Buffer): ParseResult | null {
  if (buf.length < MSG_HEADER_SIZE) return null;
  const messageLength = buf.readInt32LE(0);
  if (buf.length < messageLength) return null;

  const header: MsgHeader = {
    messageLength,
    requestId: buf.readInt32LE(4),
    responseTo: buf.readInt32LE(8),
    opCode: buf.readInt32LE(12),
  };

  if (header.opCode === OP_MSG) {
    return { frame: parseOpMsg(buf, header), consumed: messageLength };
  }
  if (header.opCode === OP_QUERY) {
    return { frame: parseOpQuery(buf, header), consumed: messageLength };
  }
  throw new Error(`unsupported opcode ${header.opCode}`);
}

function parseOpMsg(buf: Buffer, header: MsgHeader): OpMsgFrame {
  const flagBits = buf.readUInt32LE(16);
  let offset = 20;
  let body: Buffer | null = null;
  const docSequences: DocumentSequence[] = [];

  while (offset < header.messageLength) {
    const kind = buf.readUInt8(offset);
    offset += 1;
    if (kind === 0) {
      const docLen = buf.readInt32LE(offset);
      body = buf.subarray(offset, offset + docLen);
      offset += docLen;
    } else if (kind === 1) {
      const sectionLen = buf.readInt32LE(offset);
      const sectionEnd = offset + sectionLen;
      const idStart = offset + 4;
      let idEnd = idStart;
      while (idEnd < sectionEnd && buf[idEnd] !== 0) idEnd++;
      const identifier = buf.subarray(idStart, idEnd).toString('utf8');
      let docOffset = idEnd + 1;
      const docs: Buffer[] = [];
      while (docOffset < sectionEnd) {
        const dLen = buf.readInt32LE(docOffset);
        docs.push(buf.subarray(docOffset, docOffset + dLen));
        docOffset += dLen;
      }
      docSequences.push({ identifier, documents: docs });
      offset = sectionEnd;
    } else {
      throw new Error(`unknown OP_MSG section kind ${kind}`);
    }
  }

  if (!body) throw new Error('OP_MSG missing kind-0 body section');
  return { kind: 'OP_MSG', header, flagBits, body, docSequences };
}

function parseOpQuery(buf: Buffer, header: MsgHeader): OpQueryFrame {
  const flags = buf.readUInt32LE(16);
  let offset = 20;
  const nameStart = offset;
  while (offset < header.messageLength && buf[offset] !== 0) offset++;
  const fullCollectionName = buf.subarray(nameStart, offset).toString('utf8');
  offset += 1;
  const numberToSkip = buf.readInt32LE(offset);
  offset += 4;
  const numberToReturn = buf.readInt32LE(offset);
  offset += 4;
  const queryLen = buf.readInt32LE(offset);
  const query = buf.subarray(offset, offset + queryLen);
  return {
    kind: 'OP_QUERY',
    header,
    flags,
    fullCollectionName,
    numberToSkip,
    numberToReturn,
    query,
  };
}

export function encodeOpMsg(
  responseTo: number,
  requestId: number,
  bodyBson: Buffer,
  flagBits = 0,
): Buffer {
  const totalLen = MSG_HEADER_SIZE + 4 + 1 + bodyBson.length;
  const out = Buffer.allocUnsafe(totalLen);
  out.writeInt32LE(totalLen, 0);
  out.writeInt32LE(requestId, 4);
  out.writeInt32LE(responseTo, 8);
  out.writeInt32LE(OP_MSG, 12);
  out.writeUInt32LE(flagBits, 16);
  out.writeUInt8(0, 20);
  bodyBson.copy(out, 21);
  return out;
}

export function encodeOpReply(
  responseTo: number,
  requestId: number,
  docBson: Buffer,
): Buffer {
  // OP_REPLY body: responseFlags(4) + cursorID(8) + startingFrom(4) + numberReturned(4) + docs
  const totalLen = MSG_HEADER_SIZE + 4 + 8 + 4 + 4 + docBson.length;
  const out = Buffer.allocUnsafe(totalLen);
  out.writeInt32LE(totalLen, 0);
  out.writeInt32LE(requestId, 4);
  out.writeInt32LE(responseTo, 8);
  out.writeInt32LE(OP_REPLY, 12);
  out.writeUInt32LE(0, 16);
  out.writeBigInt64LE(0n, 20);
  out.writeInt32LE(0, 28);
  out.writeInt32LE(1, 32);
  docBson.copy(out, 36);
  return out;
}
