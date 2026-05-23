import fs from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';

import { MongoClient } from 'mongodb';

const repoRoot = process.cwd();
const outDir = path.join(repoRoot, 'artifacts');
const outFile = path.join(outDir, 'mongodb-target.json');

const DOCS = {
  manualSitemap: 'https://www.mongodb.com/docs/manual/sitemap-0.xml',
  commandIndex: 'https://www.mongodb.com/docs/manual/reference/command/',
  stableApi: 'https://www.mongodb.com/docs/manual/reference/stable-api/',
  stableApiChangelog:
    'https://www.mongodb.com/docs/manual/reference/stable-api-changelog/',
  manualReference: 'https://www.mongodb.com/docs/manual/reference/',
  mqlReference: 'https://www.mongodb.com/docs/manual/reference/mql/',
  commandReference: 'https://www.mongodb.com/docs/manual/reference/command/',
  opMsg: 'https://specifications.readthedocs.io/en/latest/message/OP_MSG/',
  handshake:
    'https://specifications.readthedocs.io/en/latest/mongodb-handshake/handshake/',
  auth: 'https://specifications.readthedocs.io/en/latest/auth/auth/',
  serverMonitoring:
    'https://specifications.readthedocs.io/en/latest/server-discovery-and-monitoring/server-monitoring/',
  pooling:
    'https://specifications.readthedocs.io/en/latest/connection-monitoring-and-pooling/connection-monitoring-and-pooling/',
  listCommands:
    'https://www.mongodb.com/docs/manual/reference/command/listCommands/',
};

const COMMAND_CATEGORY_HEADINGS = [
  'Aggregation Commands',
  'Query and Write Operation Commands',
  'Query Plan Cache Commands',
  'Session Commands',
  'Administration Commands',
  'Diagnostic Commands',
  'Replication Commands',
  'Sharding Commands',
  'Authentication Commands',
  'User Management Commands',
  'Role Management Commands',
  'Auditing Commands',
  'Atlas Search Commands',
];

const KNOWN_AGGREGATION_MISC = new Set([
  'interface',
  'variable',
  'variables',
  'project-fields-from-query-results',
]);

const KNOWN_AGGREGATION_STAGE_SLUGS = new Set([
  'listClusterCatalog',
  'planCacheStats',
]);

const UPDATE_OPERATOR_SYMBOLS = {
  'positional': '$',
  'positional-all': '$[]',
  'positional-filtered': '$[<identifier>]',
};

const PROJECTION_OPERATOR_SYMBOLS = {
  'positional': '$',
};

const COMMAND_FILE_BY_NAME = {
  aggregate: 'server/src/commands/aggregate.ts',
  buildInfo: 'server/src/commands/admin.ts',
  createIndexes: 'server/src/commands/createIndexes.ts',
  delete: 'server/src/commands/delete.ts',
  distinct: 'server/src/commands/distinct.ts',
  dropIndexes: 'server/src/commands/dropIndexes.ts',
  endSessions: 'server/src/commands/admin.ts',
  find: 'server/src/commands/find.ts',
  getLog: 'server/src/commands/admin.ts',
  getMore: 'server/src/commands/getMore.ts',
  hello: 'server/src/commands/hello.ts',
  insert: 'server/src/commands/insert.ts',
  killCursors: 'server/src/commands/killCursors.ts',
  listCommands: 'server/src/commands/listCommands.ts',
  listIndexes: 'server/src/commands/listIndexes.ts',
  ping: 'server/src/commands/admin.ts',
  update: 'server/src/commands/update.ts',
};

const COMMAND_ALIASES = {
  hello: ['isMaster', 'ismaster'],
  buildInfo: ['buildinfo'],
};

const STUB_COMMAND_NOTES = {
  buildInfo:
    'Static stub response; does not reflect a real storage engine, feature set, or build metadata.',
  endSessions:
    'Acknowledged as a stub; no real server-side session lifecycle is maintained.',
  getLog: 'Static stub response with an empty log payload.',
  ping: 'Trivial success response only.',
};

const PARTIAL_COMMAND_NOTES = {
  aggregate:
    'Aggregation command shell exists, but only a subset of pipeline stages/operators is implemented and many aggregate options are ignored.',
  createIndexes:
    'Index command surface is narrower than MongoDB; advanced index options are not fully modeled.',
  delete:
    'Delete command exists, but the broader MongoDB write-command option surface is not fully implemented.',
  distinct:
    'Distinct returns unique values for simple query/path cases, but options like collation, hint, readConcern, and broader BSON edge semantics are still narrower than MongoDB.',
  dropIndexes:
    'Drop-index command surface is narrower than MongoDB.',
  find:
    'Find supports filter/sort/projection/skip/limit/batchSize, but much of the official command option surface is still missing.',
  getMore:
    'Cursor continuation exists, but the full MongoDB option surface is narrower.',
  hello:
    'Handshake response advertises a MongoDB 6.0-style server, but client metadata, compression, speculative auth, and many hello request/response features are missing.',
  insert:
    'Insert accepts documents, but ordered/writeConcern/bypassDocumentValidation/comment/let semantics are incomplete.',
  killCursors:
    'Cursor kill path exists, but the broader wire/session behavior remains narrower than MongoDB.',
  listCommands:
    'listCommands is implemented from a local static registry, but the advertised command metadata surface is narrower than MongoDB.',
  listIndexes:
    'List-indexes exists, but only the current in-memory index model is exposed.',
  update:
    'Update command exists, but only a small subset of update operators and command options is implemented.',
};

const PARTIAL_STAGE_NOTES = {
  '$addFields': 'Dotted output paths are deferred, so nested writes are not MongoDB-complete.',
  '$lookup':
    'Pipeline-form lookup and let variables exist, but dotted output paths and broader lookup/index semantics are still narrower than MongoDB.',
  '$project': 'Dotted output paths are deferred, so nested projection writes are not MongoDB-complete.',
  '$set': 'Dotted output paths are deferred, so nested writes are not MongoDB-complete.',
  '$unset': 'Dotted paths are deferred.',
  '$unwind': 'Dotted paths are deferred.',
};

const PARTIAL_QUERY_NOTES = {
  '$regex':
    'Implemented through std::regex semantics, so MongoDB PCRE features and some options are still incomplete.',
};

const PARTIAL_UPDATE_NOTES = {
  '$inc': 'Top-level field updates work; dotted-path update semantics are still incomplete.',
  '$set': 'Top-level field updates work; dotted-path update semantics are still incomplete.',
  '$unset': 'Top-level field updates work; dotted-path update semantics are still incomplete.',
};

const KNOWN_GAPS = [
  {
    area: 'wire.auth',
    status: 'missing',
    note: 'No MongoDB authentication flow is implemented yet: no saslStart, saslContinue, authenticate, logout, or speculative authentication.',
    references: [DOCS.auth],
  },
  {
    area: 'wire.compression',
    status: 'missing',
    note: 'No OP_COMPRESSED path or compressor negotiation is implemented.',
    references: [DOCS.opMsg, DOCS.handshake],
  },
  {
    area: 'wire.sessions',
    status: 'partial',
    note: 'The server advertises logicalSessionTimeoutMinutes and acknowledges endSessions, but does not implement a full session model.',
    references: [DOCS.handshake],
  },
  {
    area: 'wire.transactions',
    status: 'missing',
    note: 'No transaction command flow or txnNumber/autocommit/startTransaction semantics are implemented.',
    references: [DOCS.stableApiChangelog],
  },
];

function parseArgs(argv) {
  const args = {
    mongoUri: process.env.MONGODB_URI || null,
    outFile,
  };
  for (let i = 0; i < argv.length; ++i) {
    const arg = argv[i];
    if (arg === '--mongo-uri' && argv[i + 1]) {
      args.mongoUri = argv[++i];
      continue;
    }
    if (arg.startsWith('--mongo-uri=')) {
      args.mongoUri = arg.slice('--mongo-uri='.length);
      continue;
    }
    if (arg === '--out' && argv[i + 1]) {
      args.outFile = path.resolve(repoRoot, argv[++i]);
      continue;
    }
    if (arg.startsWith('--out=')) {
      args.outFile = path.resolve(repoRoot, arg.slice('--out='.length));
    }
  }
  return args;
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function fetchText(url, attempt = 1) {
  try {
    const response = await fetch(url, {
      headers: { 'user-agent': 'savannahdb-audit/0.1 (+https://github.com)' },
    });
    if (!response.ok) {
      throw new Error(`${response.status} ${response.statusText}`);
    }
    return await response.text();
  } catch (error) {
    if (attempt >= 3) {
      throw new Error(`failed to fetch ${url}: ${error instanceof Error ? error.message : String(error)}`);
    }
    await sleep(300 * attempt);
    return fetchText(url, attempt + 1);
  }
}

function formatErrorMessage(error) {
  return error instanceof Error ? error.message : String(error);
}

function pushWarning(warnings, warning) {
  warnings.push({
    ...warning,
    severity: warning.severity || 'warning',
  });
}

async function fetchTextOrWarn(url, warnings, warningContext) {
  try {
    return await fetchText(url);
  } catch (error) {
    pushWarning(warnings, {
      type: 'fetch-failed',
      ...warningContext,
      url,
      message: formatErrorMessage(error),
    });
    return null;
  }
}

async function mapLimit(items, limit, worker) {
  const results = new Array(items.length);
  let nextIndex = 0;
  async function runWorker() {
    while (true) {
      const current = nextIndex++;
      if (current >= items.length) return;
      results[current] = await worker(items[current], current);
    }
  }
  const width = Math.min(limit, Math.max(items.length, 1));
  await Promise.all(Array.from({ length: width }, () => runWorker()));
  return results;
}

function decodeHtmlEntities(text) {
  return text
    .replace(/&#(\d+);/g, (_, code) => String.fromCharCode(Number(code)))
    .replace(/&#x([0-9a-f]+);/gi, (_, code) =>
      String.fromCharCode(Number.parseInt(code, 16)),
    )
    .replace(/&quot;/g, '"')
    .replace(/&#34;/g, '"')
    .replace(/&apos;/g, "'")
    .replace(/&#39;/g, "'")
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&amp;/g, '&')
    .replace(/&nbsp;/g, ' ');
}

function htmlToText(html) {
  const withLineBreaks = html
    .replace(/<style[\s\S]*?<\/style>/g, '')
    .replace(/<script[\s\S]*?<\/script>/g, '')
    .replace(/<!--[\s\S]*?-->/g, '')
    .replace(/<\/(tr|p|pre|div|section|tbody|table|li|ul|ol|h1|h2|h3|h4|h5|h6)>/gi, '\n')
    .replace(/<\/td>/gi, '\n')
    .replace(/<br\s*\/?>/gi, '\n');
  const stripped = withLineBreaks.replace(/<[^>]+>/g, '');
  return decodeHtmlEntities(stripped)
    .replace(/\r/g, '')
    .split('\n')
    .map((line) => line.replace(/\s+$/g, ''))
    .join('\n')
    .replace(/\n{3,}/g, '\n\n')
    .trim();
}

function extractMeta(html, attributeName, attributeValue) {
  const pattern = new RegExp(
    `<meta[^>]+${attributeName}="${attributeValue.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}"[^>]+content="([^"]*)"`,
    'i',
  );
  const match = html.match(pattern);
  return match ? decodeHtmlEntities(match[1].trim()) : null;
}

function extractCommandSyntax(html) {
  const blocks = [...html.matchAll(/<pre[^>]*>([\s\S]*?)<\/pre>/gi)]
    .map((match) => htmlToText(match[1]))
    .map((text) => text.trim())
    .filter(Boolean);
  return blocks[0] ?? null;
}

function extractTopLevelFields(syntaxSnippet) {
  if (!syntaxSnippet) return [];
  const lines = syntaxSnippet.split('\n');
  const fieldLines = [];
  let seenObjectStart = false;
  for (const line of lines) {
    if (line.includes('{')) seenObjectStart = true;
    if (!seenObjectStart || !line.includes(':')) continue;
    if (line.includes('db.runCommand')) continue;
    const match = line.match(/^(\s*)([$A-Za-z_][\w$.<>\-[\]]*)\s*:/);
    if (!match) continue;
    const [, indent, key] = match;
    fieldLines.push({
      indent: indent.length,
      key: key.trim(),
    });
  }
  if (fieldLines.length === 0) return [];
  const minIndent = Math.min(...fieldLines.map((entry) => entry.indent));
  return [...new Set(fieldLines.filter((entry) => entry.indent === minIndent).map((entry) => entry.key))];
}

function isDirectReferenceUrl(url, fragment) {
  const marker = url.indexOf(fragment);
  if (marker < 0) return false;
  const rest = url.slice(marker + fragment.length).replace(/\/+$/, '');
  return rest.length > 0 && !rest.includes('/');
}

function parseSitemapUrls(xml, fragment, { directOnly = false } = {}) {
  return [...xml.matchAll(/<loc>(.*?)<\/loc>/g)]
    .map((match) => match[1].trim())
    .filter((url) => url.includes(fragment))
    .filter((url) => (directOnly ? isDirectReferenceUrl(url, fragment) : true))
    .sort();
}

function nameFromUrl(url) {
  const clean = url.replace(/\/+$/, '');
  return clean.slice(clean.lastIndexOf('/') + 1);
}

function parseCommandCategories(indexHtml) {
  const categories = new Map();
  const positions = COMMAND_CATEGORY_HEADINGS.map((heading) => ({
    heading,
    index: indexHtml.indexOf(heading),
  }))
    .filter((entry) => entry.index >= 0)
    .sort((a, b) => a.index - b.index);

  for (let i = 0; i < positions.length; ++i) {
    const start = positions[i].index;
    const end = i + 1 < positions.length ? positions[i + 1].index : indexHtml.length;
    const section = indexHtml.slice(start, end);
    for (const match of section.matchAll(/\/docs\/manual\/reference\/command\/([^\/"#?]+)\//g)) {
      categories.set(match[1], positions[i].heading);
    }
  }
  return categories;
}

function inferAggregationKind(title, slug) {
  if (!title) {
    if (KNOWN_AGGREGATION_STAGE_SLUGS.has(slug)) return 'stage';
    return KNOWN_AGGREGATION_MISC.has(slug) ? 'misc' : 'unknown';
  }
  const lower = title.toLowerCase();
  if (lower.includes('(window operator)')) return 'window';
  if (lower.includes('(window function)')) return 'window';
  if (lower.includes('(expression operator)')) return 'expression';
  if (lower.includes('(aggregation expression)')) return 'expression';
  if (lower.includes('(accumulator operator)')) return 'accumulator';
  if (lower.includes('(aggregation accumulator)')) return 'accumulator';
  if (lower.includes('(aggregation stage)')) return 'stage';
  if (lower.includes('(aggregation)')) return 'stage';
  if (KNOWN_AGGREGATION_STAGE_SLUGS.has(slug)) return 'stage';
  if (lower.includes('aggregation operators') || lower.includes('database commands and methods')) {
    return 'misc';
  }
  return KNOWN_AGGREGATION_MISC.has(slug) ? 'misc' : 'unknown';
}

function symbolFromAggregationPage(title, slug) {
  const titleMatch = title?.match(/(\$[A-Za-z0-9_]+)/);
  if (titleMatch) return titleMatch[1];
  if (KNOWN_AGGREGATION_MISC.has(slug)) return slug;
  return `$${slug}`;
}

function symbolFromOperatorSlug(slug, mapping = {}) {
  if (mapping[slug]) return mapping[slug];
  return `$${slug}`;
}

function uniqueSorted(values) {
  return [...new Set(values)].sort((a, b) => a.localeCompare(b));
}

function extractRegexMatches(text, regex) {
  return uniqueSorted([...text.matchAll(regex)].map((match) => match[1]));
}

async function collectOfficialTarget() {
  const warnings = [];
  const [sitemapXml, commandIndexHtml] = await Promise.all([
    fetchText(DOCS.manualSitemap),
    fetchTextOrWarn(DOCS.commandIndex, warnings, {
      scope: 'command-index',
      itemType: 'command-index-page',
    }),
  ]);

  const commandUrls = parseSitemapUrls(sitemapXml, '/reference/command/', {
    directOnly: true,
  });
  const aggregationUrls = parseSitemapUrls(
    sitemapXml,
    '/reference/operator/aggregation/',
    { directOnly: true },
  );
  const queryUrls = parseSitemapUrls(sitemapXml, '/reference/operator/query/', {
    directOnly: true,
  });
  const updateUrls = parseSitemapUrls(sitemapXml, '/reference/operator/update/', {
    directOnly: true,
  });
  const projectionUrls = parseSitemapUrls(
    sitemapXml,
    '/reference/operator/projection/',
    { directOnly: true },
  );

  const commandCategories = commandIndexHtml
    ? parseCommandCategories(commandIndexHtml)
    : new Map();

  const commands = (await mapLimit(commandUrls, 8, async (url) => {
    const html = await fetchTextOrWarn(url, warnings, {
      scope: 'command-reference',
      itemType: 'command-page',
      itemName: nameFromUrl(url),
    });
    if (!html) return null;
    const slug = nameFromUrl(url);
    const title =
      extractMeta(html, 'property', 'twitter:title') ||
      extractMeta(html, 'property', 'og:title');
    const description =
      extractMeta(html, 'name', 'description') ||
      extractMeta(html, 'data-testid', 'directive-meta');
    const syntaxSnippet = extractCommandSyntax(html);
    return {
      name: slug,
      url,
      category: commandCategories.get(slug) || 'Uncategorized Commands',
      title,
      description,
      topLevelFields: extractTopLevelFields(syntaxSnippet),
      syntaxSnippet,
    };
  })).filter(Boolean);

  const aggregationPages = (await mapLimit(aggregationUrls, 8, async (url) => {
    const html = await fetchTextOrWarn(url, warnings, {
      scope: 'aggregation-reference',
      itemType: 'aggregation-page',
      itemName: nameFromUrl(url),
    });
    if (!html) return null;
    const slug = nameFromUrl(url);
    const title =
      extractMeta(html, 'property', 'twitter:title') ||
      extractMeta(html, 'property', 'og:title');
    const description =
      extractMeta(html, 'name', 'description') ||
      extractMeta(html, 'data-testid', 'directive-meta');
    const kind = inferAggregationKind(title, slug);
    return {
      name: symbolFromAggregationPage(title, slug),
      slug,
      url,
      kind,
      title,
      description,
    };
  })).filter(Boolean);

  const queryOperators = queryUrls.map((url) => ({
    name: symbolFromOperatorSlug(nameFromUrl(url)),
    slug: nameFromUrl(url),
    url,
  }));
  const updateOperators = updateUrls.map((url) => ({
    name: symbolFromOperatorSlug(nameFromUrl(url), UPDATE_OPERATOR_SYMBOLS),
    slug: nameFromUrl(url),
    url,
  }));
  const projectionOperators = projectionUrls.map((url) => ({
    name: symbolFromOperatorSlug(nameFromUrl(url), PROJECTION_OPERATOR_SYMBOLS),
    slug: nameFromUrl(url),
    url,
  }));

  const aggregationByKind = {
    stage: aggregationPages.filter((page) => page.kind === 'stage').map((page) => page.name),
    expression: aggregationPages
      .filter((page) => page.kind === 'expression')
      .map((page) => page.name),
    accumulator: aggregationPages
      .filter((page) => page.kind === 'accumulator')
      .map((page) => page.name),
    window: aggregationPages.filter((page) => page.kind === 'window').map((page) => page.name),
    misc: aggregationPages.filter((page) => page.kind === 'misc').map((page) => page.name),
  };

  return {
    sourceInventory: {
      docsManualReference: DOCS.manualReference,
      docsMqlReference: DOCS.mqlReference,
      docsCommandReference: DOCS.commandReference,
      docsStableApi: DOCS.stableApi,
      docsStableApiChangelog: DOCS.stableApiChangelog,
      docsCommandIndex: DOCS.commandIndex,
      specsOpMsg: DOCS.opMsg,
      specsHandshake: DOCS.handshake,
      specsAuth: DOCS.auth,
      specsServerMonitoring: DOCS.serverMonitoring,
      specsPooling: DOCS.pooling,
      docsListCommands: DOCS.listCommands,
    },
    fetchWarnings: warnings,
    wire: {
      compatibilityIntent: {
        targetFamily: 'MongoDB wire compatibility',
        intendedModernProtocol: 'OP_MSG',
        intendedLegacyHandshakeProtocol: 'OP_QUERY',
        serverVersionLineAdvertisedBySavannah: 'MongoDB 6.0 style wire versioning',
      },
      opcodes: [
        { name: 'OP_REPLY', code: 1, status: 'legacy reply' },
        { name: 'OP_UPDATE', code: 2001, status: 'legacy opcode' },
        { name: 'OP_INSERT', code: 2002, status: 'legacy opcode' },
        { name: 'OP_QUERY', code: 2004, status: 'legacy handshake / command opcode' },
        { name: 'OP_GET_MORE', code: 2005, status: 'legacy opcode' },
        { name: 'OP_DELETE', code: 2006, status: 'legacy opcode' },
        { name: 'OP_KILL_CURSORS', code: 2007, status: 'legacy opcode' },
        { name: 'OP_COMPRESSED', code: 2012, status: 'modern optional wrapper opcode' },
        { name: 'OP_MSG', code: 2013, status: 'modern command opcode' },
      ],
      handshake: {
        commands: ['hello', 'isMaster', 'ismaster'],
        requestFields: [
          'hello',
          'helloOk',
          'isMaster',
          'client',
          'compression',
          'loadBalanced',
          'saslSupportedMechs',
          'speculativeAuthenticate',
        ],
        responseFields: [
          'isWritablePrimary',
          'maxBsonObjectSize',
          'maxMessageSizeBytes',
          'maxWriteBatchSize',
          'logicalSessionTimeoutMinutes',
          'minWireVersion',
          'maxWireVersion',
          'connectionId',
          'topologyVersion',
          'helloOk',
          'compression',
          'saslSupportedMechs',
          'serviceId',
          'localTime',
          'ok',
        ],
        notes: [
          'hello must be sent with OP_MSG.',
          'Legacy initial handshake may use OP_QUERY with helloOk:true.',
          'Authentication follows handshake when credentials are present.',
        ],
      },
      auth: {
        commandFlow: ['saslStart', 'saslContinue', 'authenticate', 'logout'],
        mechanisms: [
          'SCRAM-SHA-256',
          'SCRAM-SHA-1',
          'MONGODB-X509',
          'MONGODB-AWS',
          'MONGODB-OIDC',
          'PLAIN',
          'GSSAPI',
        ],
      },
      commandEnvelope: {
        opMsgSections: ['kind-0 body', 'kind-1 document sequence'],
        commonDriverFields: [
          '$db',
          'lsid',
          '$readPreference',
          'apiVersion',
          'apiStrict',
          'apiDeprecationErrors',
          'txnNumber',
          'autocommit',
          'startTransaction',
          'comment',
          'maxTimeMS',
          'writeConcern',
          'readConcern',
        ],
      },
    },
    commandReference: {
      totalCommands: commands.length,
      categories: uniqueSorted(commands.map((command) => command.category)),
      commands,
    },
    aggregationReference: {
      totalPages: aggregationPages.length,
      countsByKind: Object.fromEntries(
        Object.entries(aggregationByKind).map(([kind, names]) => [kind, names.length]),
      ),
      pages: aggregationPages,
      byKind: aggregationByKind,
    },
    mqlReference: {
      queryOperators,
      updateOperators,
      projectionOperators,
    },
  };
}

function relativeFile(filePath) {
  return filePath.replace(/\\/g, '/');
}

async function readRepoFile(filePath) {
  return fs.readFile(path.join(repoRoot, filePath), 'utf8');
}

function extractBodyFields(text, variableName) {
  return uniqueSorted(
    [...text.matchAll(new RegExp(`${variableName}\\.([A-Za-z_$][A-Za-z0-9_$]*)`, 'g'))].map(
      (match) => match[1],
    ),
  );
}

function findCommandStatus(command, localInfo) {
  if (!localInfo) return { status: 'missing', note: 'No command handler is present.' };
  if (STUB_COMMAND_NOTES[command.name]) {
    return { status: 'stub', note: STUB_COMMAND_NOTES[command.name] };
  }
  if (PARTIAL_COMMAND_NOTES[command.name]) {
    return { status: 'partial', note: PARTIAL_COMMAND_NOTES[command.name] };
  }
  const officialFields = command.topLevelFields.filter((field) => field !== '<command>');
  if (officialFields.length > 0) {
    const localTopLevel = new Set(localInfo.supportedTopLevelFields);
    const unsupported = officialFields.filter((field) => !localTopLevel.has(field));
    if (unsupported.length > 0) {
      return {
        status: 'partial',
        note: `Missing documented top-level fields such as ${unsupported.slice(0, 6).join(', ')}${unsupported.length > 6 ? ', ...' : ''}.`,
      };
    }
  }
  return { status: 'implemented', note: null };
}

function buildCoverageEntries(targetNames, implementedSet, partialNotes = {}) {
  return targetNames.map((name) => ({
    name,
    status: implementedSet.has(name)
      ? partialNotes[name]
        ? 'partial'
        : 'implemented'
      : 'missing',
    note: implementedSet.has(name) ? partialNotes[name] || null : null,
  }));
}

async function collectSavannahImplementation(officialTarget) {
  const [
    wireServerText,
    parserText,
    helloText,
    aggregateText,
    distinctText,
    findText,
    updateText,
    insertText,
    deleteText,
    createIndexesText,
    dropIndexesText,
    listIndexesText,
    listCommandsText,
    getMoreText,
    killCursorsText,
    dispatchText,
    expressionText,
    groupText,
    filterText,
    updateCppText,
  ] = await Promise.all([
    readRepoFile('server/src/wire/server.ts'),
    readRepoFile('server/src/wire/parser.ts'),
    readRepoFile('server/src/commands/hello.ts'),
    readRepoFile('server/src/commands/aggregate.ts'),
    readRepoFile('server/src/commands/distinct.ts'),
    readRepoFile('server/src/commands/find.ts'),
    readRepoFile('server/src/commands/update.ts'),
    readRepoFile('server/src/commands/insert.ts'),
    readRepoFile('server/src/commands/delete.ts'),
    readRepoFile('server/src/commands/createIndexes.ts'),
    readRepoFile('server/src/commands/dropIndexes.ts'),
    readRepoFile('server/src/commands/listIndexes.ts'),
    readRepoFile('server/src/commands/listCommands.ts'),
    readRepoFile('server/src/commands/getMore.ts'),
    readRepoFile('server/src/commands/killCursors.ts'),
    readRepoFile('core/src/query/pipeline/dispatch.cpp'),
    readRepoFile('core/src/query/expression.cpp'),
    readRepoFile('core/src/query/pipeline/group.cpp'),
    readRepoFile('core/src/query/filter.cpp'),
    readRepoFile('core/src/query/update.cpp'),
  ]);

  const parserOpcodes = extractRegexMatches(parserText, /export const ([A-Z_]+) = \d+;/g);
  const parserOpcodeValues = Object.fromEntries(
    [...parserText.matchAll(/export const ([A-Z_]+) = (\d+);/g)].map((match) => [
      match[1],
      Number(match[2]),
    ]),
  );
  const dispatchCases = extractRegexMatches(wireServerText, /case '([^']+)'/g);
  const implementedCommands = new Set(Object.keys(COMMAND_FILE_BY_NAME));
  const localCommandFields = {
    aggregate: {
      supportedTopLevelFields: uniqueSorted([
        ...extractBodyFields(aggregateText, 'body'),
        'cursor.batchSize',
      ]),
      file: COMMAND_FILE_BY_NAME.aggregate,
    },
    buildInfo: {
      supportedTopLevelFields: ['buildInfo'],
      file: COMMAND_FILE_BY_NAME.buildInfo,
    },
    createIndexes: {
      supportedTopLevelFields: extractBodyFields(createIndexesText, 'body'),
      file: COMMAND_FILE_BY_NAME.createIndexes,
    },
    delete: {
      supportedTopLevelFields: uniqueSorted([
        ...extractBodyFields(deleteText, 'body'),
        'deletes',
      ]),
      file: COMMAND_FILE_BY_NAME.delete,
    },
    distinct: {
      supportedTopLevelFields: uniqueSorted([
        ...extractBodyFields(distinctText, 'body'),
        'distinct',
        'key',
        'query',
      ]),
      file: COMMAND_FILE_BY_NAME.distinct,
    },
    dropIndexes: {
      supportedTopLevelFields: extractBodyFields(dropIndexesText, 'body'),
      file: COMMAND_FILE_BY_NAME.dropIndexes,
    },
    endSessions: {
      supportedTopLevelFields: ['endSessions'],
      file: COMMAND_FILE_BY_NAME.endSessions,
    },
    find: {
      supportedTopLevelFields: extractBodyFields(findText, 'body'),
      file: COMMAND_FILE_BY_NAME.find,
    },
    getLog: {
      supportedTopLevelFields: ['getLog'],
      file: COMMAND_FILE_BY_NAME.getLog,
    },
    getMore: {
      supportedTopLevelFields: extractBodyFields(getMoreText, 'body'),
      file: COMMAND_FILE_BY_NAME.getMore,
    },
    hello: {
      supportedTopLevelFields: ['hello'],
      file: COMMAND_FILE_BY_NAME.hello,
    },
    insert: {
      supportedTopLevelFields: uniqueSorted([
        ...extractBodyFields(insertText, 'body'),
        'documents',
      ]),
      file: COMMAND_FILE_BY_NAME.insert,
    },
    killCursors: {
      supportedTopLevelFields: extractBodyFields(killCursorsText, 'body'),
      file: COMMAND_FILE_BY_NAME.killCursors,
    },
    listCommands: {
      supportedTopLevelFields: uniqueSorted([
        ...extractBodyFields(listCommandsText, 'body'),
        'listCommands',
      ]),
      file: COMMAND_FILE_BY_NAME.listCommands,
    },
    listIndexes: {
      supportedTopLevelFields: extractBodyFields(listIndexesText, 'body'),
      file: COMMAND_FILE_BY_NAME.listIndexes,
    },
    ping: {
      supportedTopLevelFields: ['ping'],
      file: COMMAND_FILE_BY_NAME.ping,
    },
    update: {
      supportedTopLevelFields: uniqueSorted([
        ...extractBodyFields(updateText, 'body'),
        'updates',
      ]),
      file: COMMAND_FILE_BY_NAME.update,
    },
  };

  const localHelloReplyFields = extractRegexMatches(
    helloText,
    /\n\s+([A-Za-z][A-Za-z0-9]+):/g,
  ).filter((field) => field !== 'counter');
  const maxWireVersionMatch = helloText.match(/const MAX_WIRE_VERSION = (\d+);/);
  const minWireVersionMatch = helloText.match(/const MIN_WIRE_VERSION = (\d+);/);
  const commandCoverage = officialTarget.commandReference.commands.map((command) => {
    const localInfo = localCommandFields[command.name];
    const status = findCommandStatus(command, localInfo);
    return {
      name: command.name,
      category: command.category,
      status: status.status,
      note: status.note,
      implementationFile: localInfo ? relativeFile(localInfo.file) : null,
      supportedTopLevelFields: localInfo ? localInfo.supportedTopLevelFields : [],
      documentedTopLevelFields: command.topLevelFields,
      aliasesSupported: COMMAND_ALIASES[command.name] || [],
    };
  });

  const stageNames = officialTarget.aggregationReference.byKind.stage;
  const expressionNames = officialTarget.aggregationReference.byKind.expression;
  const accumulatorNames = officialTarget.aggregationReference.byKind.accumulator;
  const windowNames = officialTarget.aggregationReference.byKind.window;

  const stageImplemented = new Set(
    stageNames.filter((name) => dispatchText.includes(`"${name}"`)),
  );
  const expressionImplemented = new Set(
    expressionNames.filter((name) => expressionText.includes(`"${name}"`)),
  );
  const accumulatorImplemented = new Set(
    accumulatorNames.filter(
      (name) => expressionText.includes(`"${name}"`) || groupText.includes(`"${name}"`),
    ),
  );
  const windowImplemented = new Set(
    windowNames.filter(
      (name) => expressionText.includes(`"${name}"`) || dispatchText.includes(`"${name}"`),
    ),
  );

  const implementedQueryOperators = new Set(
    officialTarget.mqlReference.queryOperators
      .map((entry) => entry.name)
      .filter((name) => filterText.includes(`"${name}"`)),
  );
  const implementedUpdateOperators = new Set(
    officialTarget.mqlReference.updateOperators
      .map((entry) => entry.name)
      .filter((name) => updateCppText.includes(`"${name}"`)),
  );
  const implementedProjectionOperators = new Set(
    officialTarget.mqlReference.projectionOperators
      .map((entry) => entry.name)
      .filter((name) => findText.includes(name) || dispatchText.includes(name)),
  );

  return {
    wire: {
      parserSupportedOpcodes: parserOpcodes.map((name) => ({
        name,
        code: parserOpcodeValues[name],
      })),
      dispatchCommandCases: dispatchCases,
      hello: {
        minWireVersion: minWireVersionMatch ? Number(minWireVersionMatch[1]) : null,
        maxWireVersion: maxWireVersionMatch ? Number(maxWireVersionMatch[1]) : null,
        aliasesAccepted: COMMAND_ALIASES.hello,
        responseFields: uniqueSorted(localHelloReplyFields),
      },
      transportStatus: [
        { feature: 'OP_MSG', status: parserText.includes('OP_MSG') ? 'implemented' : 'missing' },
        {
          feature: 'OP_QUERY',
          status: parserText.includes('OP_QUERY') ? 'implemented' : 'missing',
          note: 'Used for legacy handshake compatibility.',
        },
        {
          feature: 'OP_REPLY',
          status: parserText.includes('OP_REPLY') ? 'implemented' : 'missing',
          note: 'Used for legacy OP_QUERY replies.',
        },
        {
          feature: 'OP_COMPRESSED',
          status: 'missing',
          note: 'No compressed message handling exists in parser.ts.',
        },
      ],
    },
    commands: {
      implementedCount: [...implementedCommands].length,
      coverage: commandCoverage,
    },
    aggregation: {
      stages: buildCoverageEntries(stageNames, stageImplemented, PARTIAL_STAGE_NOTES),
      expressions: buildCoverageEntries(expressionNames, expressionImplemented),
      accumulators: buildCoverageEntries(accumulatorNames, accumulatorImplemented),
      windowOperators: buildCoverageEntries(windowNames, windowImplemented),
    },
    query: {
      operators: buildCoverageEntries(
        officialTarget.mqlReference.queryOperators.map((entry) => entry.name),
        implementedQueryOperators,
        PARTIAL_QUERY_NOTES,
      ),
    },
    update: {
      operators: buildCoverageEntries(
        officialTarget.mqlReference.updateOperators.map((entry) => entry.name),
        implementedUpdateOperators,
        PARTIAL_UPDATE_NOTES,
      ),
    },
    projection: {
      operators: buildCoverageEntries(
        officialTarget.mqlReference.projectionOperators.map((entry) => entry.name),
        implementedProjectionOperators,
      ),
    },
    knownGaps: KNOWN_GAPS,
  };
}

async function collectLiveMongoCapture(mongoUri) {
  if (!mongoUri) return null;
  const client = new MongoClient(mongoUri, {
    serverSelectionTimeoutMS: 5000,
  });
  try {
    await client.connect();
    const admin = client.db('admin');
    const [hello, buildInfo, listCommands] = await Promise.all([
      admin.command({ hello: 1 }),
      admin.command({ buildInfo: 1 }),
      admin.command({ listCommands: 1 }),
    ]);
    return {
      uri: mongoUri,
      hello,
      buildInfo,
      listCommands,
    };
  } finally {
    await client.close();
  }
}

function summarizeStatuses(entries) {
  const counts = {};
  for (const entry of entries) {
    counts[entry.status] = (counts[entry.status] || 0) + 1;
  }
  return counts;
}

function buildSummary(officialTarget, savannah) {
  return {
    official: {
      commands: officialTarget.commandReference.totalCommands,
      aggregationStages: officialTarget.aggregationReference.countsByKind.stage,
      aggregationExpressions: officialTarget.aggregationReference.countsByKind.expression,
      aggregationAccumulators: officialTarget.aggregationReference.countsByKind.accumulator,
      aggregationWindowOperators: officialTarget.aggregationReference.countsByKind.window,
      queryOperators: officialTarget.mqlReference.queryOperators.length,
      updateOperators: officialTarget.mqlReference.updateOperators.length,
      projectionOperators: officialTarget.mqlReference.projectionOperators.length,
    },
    savannahdb: {
      commands: summarizeStatuses(savannah.commands.coverage),
      aggregationStages: summarizeStatuses(savannah.aggregation.stages),
      aggregationExpressions: summarizeStatuses(savannah.aggregation.expressions),
      aggregationAccumulators: summarizeStatuses(savannah.aggregation.accumulators),
      queryOperators: summarizeStatuses(savannah.query.operators),
      updateOperators: summarizeStatuses(savannah.update.operators),
      projectionOperators: summarizeStatuses(savannah.projection.operators),
    },
  };
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const officialTarget = await collectOfficialTarget();
  const savannah = await collectSavannahImplementation(officialTarget);
  const liveMongo = await collectLiveMongoCapture(args.mongoUri);

  const artifact = {
    generatedAt: new Date().toISOString(),
    generator: {
      script: 'scripts/generate-mongodb-target.mjs',
      repoRoot: repoRoot.replace(/\\/g, '/'),
    },
    officialTarget,
    savannahdb: savannah,
    liveMongoCapture: liveMongo,
    summary: buildSummary(officialTarget, savannah),
  };

  await fs.mkdir(path.dirname(args.outFile), { recursive: true });
  await fs.writeFile(args.outFile, `${JSON.stringify(artifact, null, 2)}\n`, 'utf8');
  console.log(`wrote ${path.relative(repoRoot, args.outFile)}`);
}

await main();
