import { BSON } from 'bson';

// Minimal stubs for the admin commands the mongodb@6 driver emits during
// a normal session. Real implementations land in later phases.

export function handlePing(): Buffer {
  return Buffer.from(BSON.serialize({ ok: 1 }));
}

export function handleBuildInfo(): Buffer {
  return Buffer.from(
    BSON.serialize({
      version: '6.0.0',
      gitVersion: 'savannahdb-0.1.0',
      versionArray: [6, 0, 0, 0],
      bits: 64,
      debug: false,
      maxBsonObjectSize: 16 * 1024 * 1024,
      ok: 1,
    }),
  );
}

export function handleGetLog(): Buffer {
  return Buffer.from(
    BSON.serialize({
      totalLinesWritten: 0,
      log: [],
      ok: 1,
    }),
  );
}

export function handleEndSessions(): Buffer {
  return Buffer.from(BSON.serialize({ ok: 1 }));
}
