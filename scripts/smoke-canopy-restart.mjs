import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';

const repoRoot = process.cwd();
const storageRoot = path.join(os.tmpdir(), `savannah-canopy-${process.pid}`);

fs.rmSync(storageRoot, { recursive: true, force: true });

const commonEnv = {
  ...process.env,
  SAVANNAH_STORAGE_BACKEND: 'canopy',
  SAVANNAH_STORAGE_ROOT: storageRoot,
};

function runInline(source) {
  execFileSync(process.execPath, ['-e', source], {
    cwd: repoRoot,
    env: commonEnv,
    stdio: 'inherit',
  });
}

runInline(`
  const assert = require('node:assert/strict');
  const fs = require('node:fs');
  const path = require('node:path');
  const { BSON } = require('bson');
  const engine = require('./build/Release/savannah_engine.node');
  const ser = (value) => BSON.serialize(value);
  const de = (bytes) => BSON.deserialize(Buffer.from(bytes));
  const empty = Buffer.from([5, 0, 0, 0, 0]);

  let r = engine.createIndex('zoo', 'animals', 'species_1', 'species');
  assert.equal(r.created, true);
  engine.insert('zoo', 'animals', [
    ser({ _id: 1, species: 'lion', score: 5 }),
    ser({ _id: 2, species: null, score: 3 }),
    ser({ _id: 3, score: 4 }),
  ]);
  const update = engine.update(
    'zoo',
    'animals',
    ser({ _id: 1 }),
    ser({ $set: { score: 6 } }),
    false,
    false,
  );
  assert.equal(update.modified, 1);
  const erase = engine.erase('zoo', 'animals', ser({ _id: 2 }), true);
  assert.equal(erase.deleted, 1);

  const docs = engine.find(
    'zoo',
    'animals',
    ser({}),
    100,
    ser({ score: 1 }),
    0,
    0,
    ser({ _id: 0, species: 1, score: 1 }),
  );
  assert.deepEqual(docs.batch.map(de), [
    { score: 4 },
    { species: 'lion', score: 6 },
  ]);

  engine.insert('db', '..\\\\escape', [ser({ _id: 9, marker: 'isolated' })]);
  const alias = engine.find('escape', '', empty, 100, empty, 0, 0, empty);
  assert.deepEqual(alias.batch.map(de), []);

  const logPath = path.join(
    process.env.SAVANNAH_STORAGE_ROOT,
    'collections',
    'n-7a6f6f',
    'n-616e696d616c73',
    'ops.bin',
  );
  fs.chmodSync(logPath, 0o444);
  const failed = engine.insert('zoo', 'animals', [ser({ _id: 99, species: 'fail' })]);
  assert.ok(failed.err, 'storage append failure should surface structurally');
  const afterFailure = engine.find('zoo', 'animals', ser({ _id: 99 }), 10, empty, 0, 0, empty);
  assert.deepEqual(afterFailure.batch.map(de), []);
`);

runInline(`
  const assert = require('node:assert/strict');
  const { BSON } = require('bson');
  const engine = require('./build/Release/savannah_engine.node');
  const ser = (value) => BSON.serialize(value);
  const de = (bytes) => BSON.deserialize(Buffer.from(bytes));

  const listed = engine.listIndexes('zoo', 'animals');
  const species = listed.find((entry) => entry.name === 'species_1');
  assert.ok(species);
  assert.equal(species.entries, 2, 'missing-field row should survive restart in the nullish bucket');

  const docs = engine.find(
    'zoo',
    'animals',
    ser({}),
    100,
    ser({ species: 1 }),
    0,
    0,
    ser({ _id: 0, species: 1, score: 1 }),
  );
  assert.deepEqual(docs.batch.map(de), [
    { score: 4 },
    { species: 'lion', score: 6 },
  ]);
`);

console.log('OK — canopy restart durability');
