# MiniDB – a sharded in-memory database in C++ (with a live visual demo)

A Redis-style key-value store built from scratch: no database libraries, no HTTP libraries.
Open the web UI and **watch every command travel through the engine** – router → write-ahead log → hash table → LRU list – then press **Crash & recover** and see it rebuild itself from disk.

**Live demo:** `<your-url-here>` · **Run it locally:** see below

## Features
| | |
|---|---|
| **Sharding** | FNV-1a routes each key to one of N shards; each shard has its own lock, so different shards never contend |
| **Hash table** | 101-bucket table per shard with separate chaining (djb2 hash), written by hand |
| **LRU eviction** | Doubly linked list gives O(1) touch/evict when a shard is full |
| **TTL** | Lazy expiry on access + a background cleaner thread per shard |
| **Durability** | Write-ahead log: every mutation is flushed to disk *before* memory changes (optional `fsync`) |
| **Snapshots** | Full dump, written to a temp file and atomically renamed; WAL truncated under an exclusive lock so no write is lost in between |
| **Crash recovery** | Snapshot load + WAL replay; torn last WAL line is ignored; expired keys are never resurrected |
| **Network** | Raw TCP text protocol (`nc`/telnet) **and** an HTTP + Server-Sent-Events API for the browser |
| **Visualizer** | `web/index.html`: live pipeline, per-shard buckets and LRU order, TTL bars, WAL tail, stats, crash/recovery animation |

## Architecture
```
 browser ──HTTP/SSE──┐                          ┌─ Shard 0: buckets + LRU + TTL cleaner ─┐
 nc/telnet ──TCP─────┴─► Database (parser) ─► StorageEngine ─► Shard 1 …                │ ◄── EventBus ──► SSE ──► UI
                                      │            │          └─ Shard N                 │
                                      │            ├──► write_ahead_log (wal.log)  ◄── replay ──┐
                                      │            └──► snapshot_manager (snapshot.db) ─────────┘
```
Write path (per shard, ordered by a mutex): `route → append WAL → apply to table → move to LRU head`.

## Run it
**Linux / macOS / WSL**
```bash
cmake -S . -B build && cmake --build build -j
./build/MiniDB            # then open http://localhost:8081
```
**Windows (Visual Studio / MinGW):** the same CMake commands work (uses Winsock automatically).
**Docker:** `docker build -t minidb . && docker run -p 8081:8081 minidb`

Raw TCP: `nc localhost 8080` → `SET name "Ada Lovelace" 60`, `GET name`, `HELP`.

## Commands
`SET key value [ttl_seconds]` · `GET key` · `DEL key` · `TTL key` · `SNAPSHOT` · `INFO` · `FLUSH` · `PING` · `QUIT`
Values with spaces need quotes: `SET greeting "hello world"`.

## HTTP API
`GET /` UI · `GET /api/state` full state · `GET /api/wal` · `POST /api/cmd` (body = command) · `POST /api/crash` · `GET /api/events` (SSE stream)

## Configuration (environment variables)
`PORT`/`MINIDB_HTTP_PORT` (8081) · `MINIDB_TCP_PORT` (8080, `0` = off) · `MINIDB_SHARDS` (4) · `MINIDB_CAPACITY` keys per shard (4 – tiny on purpose so evictions are visible) · `MINIDB_DATA_DIR` (`data`) · `MINIDB_FSYNC=1` · `MINIDB_WEB_DIR` (`web`)

## Tests
```bash
./build/FunctionalTests   # 26 checks: TTL, LRU order, WAL/snapshot recovery, torn WAL, parser, limits
./build/StressTest 8 5000 # 8 threads, verifies every value, restarts from disk, verifies again
```
Reference run (laptop-class CPU, WAL flushed on every write, no fsync): ~500k ops/s, p50 ≈ 1 µs, p99 ≈ 8 µs, recovery of 40k keys ≈ 40 ms. The stress test also runs clean under ThreadSanitizer (`-fsanitize=thread`).

## Deploy the live demo (free)
1. Push this folder to GitHub. 2. On [Render](https://render.com): *New → Blueprint* → pick the repo (`render.yaml` is included), or *New → Web Service → Docker*.
3. Put the URL at the top of this README. Free tiers sleep when idle and have a temporary disk, so data resets on restart – fine for a demo. Check the host's current free-tier terms.
The demo is protected by key/value size limits, a per-client rate limit, a cap on viewers and auto-snapshot when the WAL passes 4 MB. All visitors share one database.

## Design notes & known limits
- Reads take the shard's exclusive lock because a read reorders the LRU list (a `shared_mutex` would not help).
- Expiry uses wall-clock time (`system_clock`) so it survives restarts; a clock jump affects TTLs.
- Single-node, no replication, no auth on the TCP port – it's a learning/portfolio project, not a Redis replacement.
- Ideas: per-visitor sandboxes, RESP protocol compatibility, background snapshots, B-tree secondary index, `fsync` batching (group commit).
