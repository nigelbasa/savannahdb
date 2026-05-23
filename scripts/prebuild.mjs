// Build the native addon for the current platform/arch and copy the resulting
// .node into server/prebuilds/<platform>-<arch>/node.napi.node, which is what
// node-gyp-build looks for at install time.
//
// Run after the CMake build has populated build/Release/savannah_engine.node.
// Used by the prebuild CI matrix; safe to run locally too.

import { spawnSync } from 'node:child_process';
import { mkdirSync, copyFileSync, existsSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, '..');

function run(cmd, args) {
  const result = spawnSync(cmd, args, {
    cwd: repoRoot,
    stdio: 'inherit',
    shell: process.platform === 'win32',
  });
  if (result.status !== 0) {
    process.exit(result.status ?? 1);
  }
}

const sourceAddon = resolve(repoRoot, 'build', 'Release', 'savannah_engine.node');

if (!existsSync(sourceAddon)) {
  console.log('Building native addon via npm run build:native…');
  run('npm', ['run', 'build:native']);
}

if (!existsSync(sourceAddon)) {
  console.error(`Native build did not produce ${sourceAddon}`);
  process.exit(1);
}

// Translate Node's process.platform / process.arch to node-gyp-build's
// directory naming. Same triplet convention used by prebuild-install,
// prebuildify, and the rest of the ecosystem.
const platform = process.platform;
const arch = process.argv[2] || process.arch;
const target = resolve(repoRoot, 'server', 'prebuilds', `${platform}-${arch}`);
mkdirSync(target, { recursive: true });

const dest = resolve(target, 'node.napi.node');

// Windows keeps an exclusive handle on any loaded .node, so opening dest
// for write fails with EBUSY if another process still has the previous
// prebuild mapped. The OS *does* allow renaming a locked file, though —
// the open handles stay attached to the renamed inode. So we rotate the
// existing file out of the way (it'll be cleaned up by the next CI run or
// a manual sweep) and then copy the fresh build into the canonical name.
import { renameSync } from 'node:fs';

if (existsSync(dest)) {
  try {
    renameSync(dest, `${dest}.stale-${Date.now()}`);
  } catch (err) {
    if (err.code !== 'ENOENT') throw err;
  }
}
copyFileSync(sourceAddon, dest);
console.log(`Prebuild written: ${dest}`);
