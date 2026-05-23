# SavannahDB Artifacts

This directory currently contains three working artifacts:

- `mongodb-target.json`: the machine-readable compatibility baseline for
  SavannahDB
- `compare-driver-report.json`: output from the side-by-side harness in
  `scripts/compare-driver.mjs`
- `storage-engine-spec.md`: the normative contract for SavannahDB's permanent
  storage backend

`mongodb-target.json` combines three views:

- `officialTarget`: MongoDB command/operator inventory scraped from the official MongoDB docs and specifications.
- `savannahdb`: a local static-analysis pass over this repo that marks what is implemented, partial, stubbed, or missing.
- `liveMongoCapture`: optional runtime capture from a real MongoDB server (`hello`, `buildInfo`, `listCommands`) when you provide a URI.

## Regenerate

Without a live MongoDB server:

```bash
npm run generate:mongodb-target
```

With a live MongoDB server:

```bash
npm run generate:mongodb-target -- --mongo-uri mongodb://127.0.0.1:27018
```

You can also override the output path:

```bash
node scripts/generate-mongodb-target.mjs --out artifacts/mongodb-target.local.json
```

## Side-by-side Harness

Run SavannahDB against a real MongoDB server:

```bash
npm run compare:driver -- --savannah-uri mongodb://127.0.0.1:27017/?directConnection=true --mongo-uri mongodb://127.0.0.1:27018/?directConnection=true
```

Filter to a feature area:

```bash
npm run compare:driver -- --tag aggregation
npm run compare:driver -- --tag gap
```

The harness writes `artifacts/compare-driver-report.json` with:

- per-case result status (`match`, `mismatch`, `known-gap-observed`, `known-gap-resolved`)
- canonicalized SavannahDB and MongoDB outputs
- feature tags and coverage references
- summary counts for quick CI or local review

## Notes

- The docs scrape is intentionally broad, especially for aggregation and command coverage.
- `listCommands` data is only available in `liveMongoCapture` when a real MongoDB server is reachable.
- Command status in `savannahdb.commands.coverage` is conservative:
  - `stub` means a minimal fixed reply exists.
  - `partial` means the command exists but the documented option surface is narrower than MongoDB.
  - `missing` means there is no command handler in the current wire server.
