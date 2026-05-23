import fs from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';
import { isDeepStrictEqual } from 'node:util';

import { EJSON } from 'bson';
import { MongoClient } from 'mongodb';
import { makeCases } from './compare-driver/cases.mjs';

const DEFAULT_SAVANNAH_URI =
  process.env.SAVANNAH_URI ||
  'mongodb://127.0.0.1:27017/?serverSelectionTimeoutMS=3000&directConnection=true';
const DEFAULT_MONGO_URI =
  process.env.MONGODB_URI ||
  'mongodb://127.0.0.1:27018/?serverSelectionTimeoutMS=3000&directConnection=true';
const DEFAULT_OUT = path.join(process.cwd(), 'artifacts', 'compare-driver-report.json');
const DEFAULT_DB_PREFIX = 'compare_driver';

function parseArgs(argv) {
  const args = {
    savannahUri: DEFAULT_SAVANNAH_URI,
    mongoUri: DEFAULT_MONGO_URI,
    outFile: DEFAULT_OUT,
    dbPrefix: DEFAULT_DB_PREFIX,
    tagFilters: [],
    caseFilters: [],
  };

  for (let i = 0; i < argv.length; ++i) {
    const arg = argv[i];
    if (arg === '--savannah-uri' && argv[i + 1]) {
      args.savannahUri = argv[++i];
      continue;
    }
    if (arg.startsWith('--savannah-uri=')) {
      args.savannahUri = arg.slice('--savannah-uri='.length);
      continue;
    }
    if (arg === '--mongo-uri' && argv[i + 1]) {
      args.mongoUri = argv[++i];
      continue;
    }
    if (arg.startsWith('--mongo-uri=')) {
      args.mongoUri = arg.slice('--mongo-uri='.length);
      continue;
    }
    if (arg === '--out' && argv[i + 1]) {
      args.outFile = path.resolve(process.cwd(), argv[++i]);
      continue;
    }
    if (arg.startsWith('--out=')) {
      args.outFile = path.resolve(process.cwd(), arg.slice('--out='.length));
      continue;
    }
    if (arg === '--db-prefix' && argv[i + 1]) {
      args.dbPrefix = argv[++i];
      continue;
    }
    if (arg.startsWith('--db-prefix=')) {
      args.dbPrefix = arg.slice('--db-prefix='.length);
      continue;
    }
    if (arg === '--tag' && argv[i + 1]) {
      args.tagFilters.push(argv[++i]);
      continue;
    }
    if (arg.startsWith('--tag=')) {
      args.tagFilters.push(arg.slice('--tag='.length));
      continue;
    }
    if (arg === '--case' && argv[i + 1]) {
      args.caseFilters.push(argv[++i]);
      continue;
    }
    if (arg.startsWith('--case=')) {
      args.caseFilters.push(arg.slice('--case='.length));
    }
  }

  return args;
}

function slugify(text) {
  return text.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '');
}

function canonicalize(value) {
  const serialized = EJSON.serialize(value, { relaxed: false });
  return sortKeys(serialized);
}

function sortKeys(value) {
  if (Array.isArray(value)) return value.map(sortKeys);
  if (value && typeof value === 'object') {
    return Object.fromEntries(
      Object.keys(value)
        .sort((left, right) => left.localeCompare(right))
        .map((key) => [key, sortKeys(value[key])]),
    );
  }
  return value;
}

function normalizeError(error) {
  const out = {
    name: error?.name || 'Error',
    message: error?.message ? String(error.message) : String(error),
  };
  if (typeof error?.code === 'number') out.code = error.code;
  if (typeof error?.codeName === 'string') out.codeName = error.codeName;
  return out;
}

async function captureResult(run) {
  try {
    const value = await run();
    return { ok: true, value: canonicalize(value) };
  } catch (error) {
    return { ok: false, error: normalizeError(error) };
  }
}

function summarizeStatuses(cases) {
  const counts = {};
  for (const item of cases) {
    counts[item.status] = (counts[item.status] || 0) + 1;
  }
  return counts;
}

async function loadCoverageArtifact() {
  try {
    const text = await fs.readFile(
      path.join(process.cwd(), 'artifacts', 'mongodb-target.json'),
      'utf8',
    );
    const data = JSON.parse(text);
    return {
      commands: new Map(
        (data.savannahdb?.commands?.coverage || []).map((entry) => [entry.name, entry]),
      ),
      aggStages: new Map(
        (data.savannahdb?.aggregation?.stages || []).map((entry) => [entry.name, entry]),
      ),
      aggExprs: new Map(
        (data.savannahdb?.aggregation?.expressions || []).map((entry) => [entry.name, entry]),
      ),
      aggAccums: new Map(
        (data.savannahdb?.aggregation?.accumulators || []).map((entry) => [entry.name, entry]),
      ),
      queryOps: new Map(
        (data.savannahdb?.query?.operators || []).map((entry) => [entry.name, entry]),
      ),
      updateOps: new Map(
        (data.savannahdb?.update?.operators || []).map((entry) => [entry.name, entry]),
      ),
      projectionOps: new Map(
        (data.savannahdb?.projection?.operators || []).map((entry) => [entry.name, entry]),
      ),
    };
  } catch {
    return null;
  }
}

function coverageRefsForCase(coverage, refs) {
  if (!coverage || !refs?.length) return [];
  const out = [];
  for (const ref of refs) {
    const [kind, name] = ref.split(':', 2);
    let entry = null;
    if (kind === 'command') entry = coverage.commands.get(name);
    if (kind === 'agg-stage') entry = coverage.aggStages.get(name);
    if (kind === 'agg-expr') entry = coverage.aggExprs.get(name);
    if (kind === 'agg-accum') entry = coverage.aggAccums.get(name);
    if (kind === 'query-op') entry = coverage.queryOps.get(name);
    if (kind === 'update-op') entry = coverage.updateOps.get(name);
    if (kind === 'projection-op') entry = coverage.projectionOps.get(name);
    out.push({
      ref,
      status: entry?.status ?? 'unknown',
      note: entry?.note ?? null,
    });
  }
  return out;
}

function filterCases(cases, args) {
  return cases.filter((testCase) => {
    if (args.caseFilters.length > 0 && !args.caseFilters.includes(testCase.name)) {
      return false;
    }
    if (
      args.tagFilters.length > 0 &&
      !args.tagFilters.every((tag) => testCase.tags.includes(tag))
    ) {
      return false;
    }
    return true;
  });
}

async function runCase(testCase, clients, runId, dbPrefix, coverageArtifact) {
  const shortPrefix = slugify(dbPrefix).slice(0, 8) || 'cmp';
  const shortCase = slugify(testCase.name).slice(0, 20) || 'case';
  const shortRun = runId.slice(-6);
  const dbNames = {
    savannah: `${shortPrefix}_${shortCase}_${shortRun}_s`,
    mongo: `${shortPrefix}_${shortCase}_${shortRun}_m`,
  };
  const startedAt = Date.now();
  const contexts = {
    savannah: {
      client: clients.savannah,
      admin: clients.savannah.db('admin'),
      db: clients.savannah.db(dbNames.savannah),
      dbName: dbNames.savannah,
      target: 'savannah',
    },
    mongo: {
      client: clients.mongo,
      admin: clients.mongo.db('admin'),
      db: clients.mongo.db(dbNames.mongo),
      dbName: dbNames.mongo,
      target: 'mongo',
    },
  };

  const [savannahResult, mongoResult] = await Promise.all([
    captureResult(() => testCase.run(contexts.savannah)),
    captureResult(() => testCase.run(contexts.mongo)),
  ]);
  const equal = isDeepStrictEqual(savannahResult, mongoResult);

  let status = 'match';
  if (testCase.expectation === 'match') {
    status = equal ? 'match' : 'mismatch';
  } else if (testCase.expectation === 'known-gap') {
    status = equal ? 'known-gap-resolved' : 'known-gap-observed';
  }

  return {
    name: testCase.name,
    expectation: testCase.expectation,
    tags: testCase.tags,
    featureRefs: testCase.featureRefs ?? [],
    featureCoverage: coverageRefsForCase(coverageArtifact, testCase.featureRefs ?? []),
    dbNames,
    durationMs: Date.now() - startedAt,
    status,
    equal,
    savannah: savannahResult,
    mongo: mongoResult,
  };
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const runId = new Date().toISOString().replace(/[-:.TZ]/g, '').slice(0, 14);
  const coverageArtifact = await loadCoverageArtifact();
  const cases = filterCases(makeCases(), args);

  if (cases.length === 0) {
    console.error('No cases matched the requested filters.');
    process.exitCode = 1;
    return;
  }

  const clients = {
    savannah: new MongoClient(args.savannahUri),
    mongo: new MongoClient(args.mongoUri),
  };

  try {
    await Promise.all([clients.savannah.connect(), clients.mongo.connect()]);
    console.log(`connected savannah=${args.savannahUri}`);
    console.log(`connected mongo=${args.mongoUri}`);

    const results = [];
    for (const testCase of cases) {
      const result = await runCase(
        testCase,
        clients,
        runId,
        args.dbPrefix,
        coverageArtifact,
      );
      results.push(result);
      console.log(
        `${result.status.toUpperCase()} ${result.name} (${result.durationMs}ms)`,
      );
    }

    const summary = {
      comparedAt: new Date().toISOString(),
      savannahUri: args.savannahUri,
      mongoUri: args.mongoUri,
      dbPrefix: args.dbPrefix,
      selectedCases: cases.map((testCase) => testCase.name),
      counts: summarizeStatuses(results),
    };

    const report = {
      summary,
      cases: results,
    };

    await fs.mkdir(path.dirname(args.outFile), { recursive: true });
    await fs.writeFile(args.outFile, `${JSON.stringify(report, null, 2)}\n`, 'utf8');
    console.log(`wrote ${path.relative(process.cwd(), args.outFile)}`);

    const hasMismatch = results.some((entry) => entry.status === 'mismatch');
    if (hasMismatch) process.exitCode = 1;
  } finally {
    await Promise.allSettled([clients.savannah.close(), clients.mongo.close()]);
  }
}

await main();
