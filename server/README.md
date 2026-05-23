# SavannahDB 🦁

SavannahDB is an independent, high-performance, lightweight document database. Written with a robust C++ engine core (the "Jungle" storage and indexing layer), it is exposed through a unified Node.js SDK and a standalone JSON-over-HTTP REST micro-server.

Instead of mimicking legacy wire protocols, SavannahDB is built from the ground up for modern developers who need either a lightweight **in-process embedded database** (like SQLite) or a **standalone HTTP micro-database** (like CouchDB) that can be consumed by any language.

---

## ✨ Key Features

* **Embedded Mode (In-Process)**: Run SavannahDB directly inside your Node.js application process with zero network latency. Perfect for serverless environments, local apps, and fast testing.
* **REST Server Mode (Client-Server)**: Run SavannahDB as a standalone micro-server. It exposes a clean HTTP/JSON REST API, allowing Node.js, Python, Go, Rust, or any other language to interact with it seamlessly.
* **Durable Persistence**: Supports a highly resilient log-structured Canopy/LMDB backend on disk, featuring durable append-only write-ahead logging (WAL) and atomic state checkpointing.
* **B-Tree Indexing**: Automatic index backfilling and B-Tree indexing on any field (including nested paths) for up to **2,000% speedups** on queries.
* **Native C++ Aggregation Pipelines**: Multi-stage native aggregations ($match, $group, $lookup, $unwind, etc.) executing at C++ native speed.

---

## 🚀 Quick Start

### 1. Installation
```bash
npm install @nigelbasa/savannahdb
```

Prebuilt native binaries ship for common platforms (Windows x64, Linux x64, macOS x64/arm64). No toolchain is required at install time. For unsupported platforms, see *Building from source* at the bottom.

### 2. Direct Embedded Mode (In-Process)
Use SavannahDB directly in Node.js with zero server setup:

```javascript
import { SavannahDB } from '@nigelbasa/savannahdb';

// Initialize in-process database (runs in Canopy persistent mode)
const db = new SavannahDB({
  storage: {
    backend: 'canopy',
    root: './data/my_db'
  }
});

const animals = db.collection('zoo', 'animals');

// Insert documents
await animals.insertMany([
  { _id: 1, name: 'giraffe', height: 5.5, tags: ['tall'] },
  { _id: 2, name: 'lion', pack: ['simba'], height: 1.2 }
]);

// Chained find queries with B-Tree indices
const tallAnimals = await animals.find({ height: { $gt: 2.0 } })
  .sort({ height: -1 })
  .toArray();

console.log(tallAnimals);
```

### 3. Server Mode (Client-Server REST API)
1. **Start the standalone server**:
   ```bash
   npm start
   ```
   *The server starts listening on port `27018` by default.*

2. **Connect using Node.js SDK**:
   ```javascript
   import { SavannahDB } from '@nigelbasa/savannahdb';

   const db = new SavannahDB({
     url: 'http://localhost:27018'
   });

   const animals = db.collection('zoo', 'animals');
   const lion = await animals.findOne({ name: 'lion' });
   ```

3. **Connect using Python (or any language over HTTP)**:
   ```python
   import requests

   res = requests.post(
       "http://localhost:27018/api/v1/zoo/animals/find",
       json={"filter": {"height": {"$gt": 2.0}}, "sort": {"height": -1}}
   )
   print(res.json())  # Returns {"batch": [...], "cursorId": "0"}
   ```

---

## 📖 Comprehensive Documentation

For the complete API reference, list of supported query filters, update modifiers, and a full guide to the native aggregation stages and expression operators, see:

👉 **[SavannahDB Exhaustive Command & API Reference](docs.md)**
