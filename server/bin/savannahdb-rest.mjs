#!/usr/bin/env node
// CLI entrypoint: starts the SavannahDB REST server.
// Configuration is via env vars (PORT, SAVANNAH_HOST, SAVANNAH_AUTH_TOKEN,
// SAVANNAH_STORAGE_BACKEND, SAVANNAH_STORAGE_ROOT) — see README.

import { startServer } from '../dist/server/index.js';

const port = Number(process.env.PORT ?? 27018);
startServer({ port });
