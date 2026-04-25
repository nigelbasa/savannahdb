import { BSON, Long, ObjectId } from 'bson';
import type { Session } from '../wire/session.js';

// Target wire version: MongoDB 6.0 (maxWireVersion 17).
const MAX_WIRE_VERSION = 17;
const MIN_WIRE_VERSION = 0;
const MAX_BSON_OBJECT_SIZE = 16 * 1024 * 1024;
const MAX_MESSAGE_SIZE_BYTES = 48_000_000;
const MAX_WRITE_BATCH_SIZE = 100_000;

const processId = new ObjectId();

export function handleHello(session: Session): Buffer {
  const response = {
    isWritablePrimary: true,
    topologyVersion: { processId, counter: Long.fromNumber(0) },
    maxBsonObjectSize: MAX_BSON_OBJECT_SIZE,
    maxMessageSizeBytes: MAX_MESSAGE_SIZE_BYTES,
    maxWriteBatchSize: MAX_WRITE_BATCH_SIZE,
    localTime: new Date(),
    logicalSessionTimeoutMinutes: 30,
    connectionId: session.connectionId,
    minWireVersion: MIN_WIRE_VERSION,
    maxWireVersion: MAX_WIRE_VERSION,
    readOnly: false,
    ok: 1,
  };
  return Buffer.from(BSON.serialize(response));
}
