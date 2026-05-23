# SavannahDB Permanent Storage Engine Specification

Status: draft, normative

This document is the storage-engine contract for SavannahDB's permanent
backend. It is intentionally written as a specification, not a survey of
candidate engines. It defines the behavior the engine MUST provide, the
behavior it SHOULD provide as it matures, and the behavior that MAY be added
later without changing the contract.

The goal is to make Phase 0.5 and later storage work measurable. "Done" means
"meets this spec," not "uses a particular library."

## 1. Normative Language

The key words `MUST`, `MUST NOT`, `SHOULD`, `SHOULD NOT`, and `MAY` are to be
interpreted as requirements strength:

- `MUST` / `MUST NOT`: required for the first acceptable permanent engine.
- `SHOULD` / `SHOULD NOT`: strongly preferred; may be deferred if the design
  keeps the door open.
- `MAY`: optional enhancement.

## 2. Scope

This spec covers the local persistent storage layer behind SavannahDB's current
Jungle storage seam:

- `savannah::storage::IStorageBackend`
- `savannah::jungle::storage::v1::Collection`
- iterator behavior consumed by `find`, `aggregate`, and `getMore`
- persistent collection catalog and secondary indexes
- crash recovery, maintenance, and observability

This spec does not require replication, sharding, distributed transactions, or
networked storage coordination.

## 3. Primary Goals

The permanent engine exists to provide:

1. Correctness before cleverness.
2. Durable state across process restart and host restart.
3. Stable query semantics that exactly match the engine's logical comparators.
4. Predictable recovery from crashes and partial failures.
5. A concurrency model that allows reads and writes to proceed without breaking
   cursor or iterator guarantees.

## 4. Required Data Model Features

### 4.1 Persistent State

The engine `MUST` persist all of the following:

- collection existence and namespace metadata
- index definitions and index state
- document bytes
- any metadata required to reopen the database consistently after restart
- storage-format version metadata

The engine `MUST NOT` rely on process memory as the source of truth after a
commit is acknowledged.

### 4.2 Atomic Document and Index Updates

The engine `MUST` treat a logical write as an atomic unit across:

- primary document storage
- all affected secondary indexes
- collection metadata touched by the operation

After crash recovery, SavannahDB `MUST NOT` observe:

- a document that exists without the corresponding committed index entries
- index entries that point to an uncommitted or missing document
- partially applied replacements or updates

`insert`, `update`, `erase`, and index backfill state changes `MUST` be
recoverable without manual repair for ordinary crashes.

### 4.3 Exact Query Ordering Semantics

The engine `MUST` preserve the same value ordering used by the query layer.
On-disk or persisted index key encoding `MUST` agree with a single explicit
key-order contract shared by scan sort, index comparators, and durable key
encoding.

In the current codebase that contract is centered on:

- `query/key_order.*` for missing/null behavior and sort-facing ordering rules
- `query/value.*` for same-value equality and comparable BSON-type ordering

Future refactors `MAY` move or rename those files, but the engine `MUST NOT`
allow separate ad-hoc ordering logic to grow in scan sort, index storage, or
durable key encoding.

This includes exact handling of:

- mixed BSON types
- `null`
- missing fields where SavannahDB intentionally treats them as equivalent in
  sort order
- strings and string comparison rules already implemented by the engine
- arrays and objects as they participate in current and future index semantics
- stable tie-break order for equal sort keys

Equal sort keys `MUST` break ties by stable logical document identity rather
than by incidental physical placement. SavannahDB's durable storage design
should treat `RecordId` (or its future equivalent) as that stable tie-break.

If the persisted sort or index order disagrees with the in-memory comparator,
the storage engine is non-compliant even if basic CRUD appears to work.

## 5. Concurrency and Isolation Requirements

### 5.1 Snapshot Reads

Read operations `MUST` observe a stable snapshot for the lifetime of the read
operation or cursor view.

At minimum:

- a single `find` call `MUST` not observe the same document twice because of a
  concurrent write
- a cursor resumed by `getMore` `MUST` continue from a defined visibility point
- an aggregation pipeline `MUST` not mix pre-update and post-update views of
  the same logical document within one pipeline execution unless the command
  contract explicitly allows it

The storage engine `SHOULD` implement snapshot semantics directly rather than
simulating them with full materialization when avoidable.

### 5.2 Reader and Writer Interaction

The engine `MUST` allow multiple concurrent readers.

The engine `SHOULD` allow readers to proceed without blocking on ordinary
writes.

The engine `SHOULD` allow writes to different documents to proceed in parallel.
This is the desired "document-level write concurrency" target. If the first
durable backend cannot provide it, the design `MUST` still leave room to add
it later without changing the logical storage contract.

### 5.3 Write Conflicts

When concurrent writes cannot both succeed, the engine `MUST` surface a clean
conflict outcome internally rather than allowing silent lost updates or index
corruption.

The engine `SHOULD` support transparent retry for safe internal conflicts, but
retry behavior `MUST NOT` violate command semantics.

## 6. Durability Requirements

### 6.1 Commit Semantics

The engine `MUST` define what "commit acknowledged" means.

SavannahDB `SHOULD` support at least these durability modes over time:

- process-acknowledged: accepted into the engine but not guaranteed durable
- durable-acknowledged: protected by WAL, journal, copy-on-write commit, or an
  equivalent crash-recovery mechanism
- fully-synced: data and required metadata forced to stable storage before ack

If only one durability mode exists initially, it `MUST` be documented and it
`MUST` be crash-safe for acknowledged writes.

### 6.2 Recovery

After an unclean shutdown, the engine `MUST`:

- reopen without manual repair for expected crash cases
- recover to the last valid committed state
- avoid exposing torn metadata or torn user-visible writes
- detect unrecoverable corruption and fail closed rather than returning
  plausible-looking incorrect data

The engine `SHOULD` record enough metadata to make startup recovery bounded and
explainable.

### 6.3 Integrity

The engine `SHOULD` provide checksums or equivalent corruption detection for
critical persisted structures.

The engine `MUST` never silently treat obviously malformed persisted state as
valid BSON documents or index entries.

## 7. Cursor and Iterator Requirements

The storage contract is iterator-based today, so the permanent engine `MUST`
preserve that behavior.

Required properties:

- cursor IDs remain a wire-layer concern; storage `MUST` only guarantee stable
  iterator semantics for the registered iterator lifetime
- iterators `MUST` not rely on mutable process-memory slot addresses as the
  durable source of truth
- a live iterator `MUST` either:
  - continue against its original snapshot, or
  - fail in a defined, structural way if the engine explicitly does not support
    snapshot continuation for that iterator class

The engine `SHOULD` prefer snapshot continuation so `getMore` remains simple
and Mongo-like.

## 8. Index Requirements

### 8.1 Secondary Indexes

The engine `MUST` persist secondary indexes.

The engine `MUST` support:

- create index definition persistence
- index backfill or build persistence
- index visibility rules that prevent half-built indexes from being treated as
  valid by the planner
- index drop that removes both metadata and durable index contents safely

### 8.2 Planner Trust

If the planner chooses an index-backed lookup or sort, the result `MUST` match
the scan path. Any difference between index-backed and scan-backed results is a
storage correctness bug.

### 8.3 Rebuild and Validation

The engine `SHOULD` support:

- explicit index rebuild
- index validation or consistency checking
- bounded restart behavior when an index was building during crash

## 9. Maintenance Requirements

The engine `MUST` have a maintenance model that is explicit, observable, and
safe.

Depending on the underlying design, that may include flushing, checkpoints,
space reclamation, or compaction. The spec does not mandate a specific
mechanism, but it does mandate the outcomes.

Required outcomes:

- committed data eventually reaches durable steady state
- old garbage, obsolete versions, or dead pages can be reclaimed
- maintenance `MUST NOT` require the database to go offline for normal
  operation
- maintenance work `SHOULD` avoid long stop-the-world pauses

The engine `SHOULD` support online backup or snapshot export without requiring
the server to stop accepting reads.

## 10. Observability Requirements

The engine `SHOULD` expose counters or diagnostics for:

- recovery duration
- dirty or not-yet-checkpointed data volume, if applicable
- write conflict counts
- active readers or snapshots, if applicable
- index build or backfill progress
- storage size by collection and index
- maintenance backlog, flush backlog, or compaction backlog, if applicable

If the engine has a tunable durability or maintenance model, those settings
`MUST` be introspectable.

## 11. Format and Upgrade Requirements

The engine `MUST` version its on-disk format.

The engine `MUST` either:

- reject unsupported format versions clearly, or
- migrate them intentionally

The engine `MUST NOT` silently reinterpret an incompatible format.

Format upgrades `SHOULD` be planned so future changes do not force a complete
rewrite of the storage seam.

## 12. Non-Goals for the First Permanent Engine

The first compliant permanent engine does not need to provide:

- replication
- sharding
- text or geo indexing
- multi-document ACID transactions
- encrypted-at-rest storage
- online schema migration for arbitrary future formats

These may be added later, but they are not blockers for initial compliance.

## 13. SavannahDB-Specific Implementation Constraints

Any permanent backend implementation `MUST` preserve these architectural
constraints from the current codebase:

- `IStorageBackend` remains the swap point between engine and storage
- query, pipeline, planner, and expression layers `MUST NOT` reach into
  backend-private document slot layouts
- backend-owned index backfill remains behind the collection interface
- `aggregate` continues to return owned result bytes where stage output no
  longer aliases base storage
- storage-visible behavior stays structural; expected failures are returned
  through result structs rather than thrown as cross-boundary exceptions

## 14. Acceptance Criteria

The permanent engine is acceptable when all of the following are true:

### 14.1 Restart and Recovery

- insert documents, restart, and observe the same documents
- update documents, restart, and observe the updated documents
- delete documents, restart, and observe the deletions
- create indexes, restart, and observe them still present and usable
- if the process is terminated during write load, restart recovers to a valid
  committed state without manual repair

### 14.2 Query Correctness

- the side-by-side SavannahDB vs MongoDB harness continues to pass all current
  strict parity cases that are not explicitly outside SavannahDB scope
- index-backed lookups and sorts match scan-backed results
- mixed-type sorts and null-or-missing behavior remain identical between scan
  and index paths

### 14.3 Cursor Correctness

- a cursor opened before unrelated writes can continue via `getMore` without
  duplicate or skipped committed rows within its defined snapshot semantics
- cursor lifetime does not depend on mutable in-memory slot addresses surviving
  restart or compaction

### 14.4 Operational Safety

- storage format version is visible and checked on open
- corruption or incompatible format is rejected clearly
- maintenance activity does not require manual downtime for normal CRUD usage

## 15. Recommended Delivery Order

To minimize risk, implementation should satisfy this spec in the following
order:

1. Persistent catalog and document storage.
2. Atomic secondary-index persistence.
3. Restart-safe CRUD and `find`.
4. Snapshot-safe cursor continuation.
5. Observability and maintenance tooling.
6. Higher-concurrency write behavior.

That order is guidance, not a separate requirement, but it reflects the
correctness-first intent of this spec.
