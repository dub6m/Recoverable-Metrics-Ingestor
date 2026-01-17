# Recoverable Metrics Ingestor

## Overview
The ingestor uses a **Write-Ahead Log (WAL)** plus an in-memory **MemStore** to balance ingestion speed with crash recoverability.

- **WAL**: an append-only log of metric writes. It is the source of truth for rebuilds after a crash, keeping a sequential history of updates in the order they were accepted.
- **MemStore**: an in-memory map that serves fast reads and aggregates current metric state for the running process.

During normal operation, writes are appended to the WAL and then applied to the MemStore. The MemStore is optimized for low-latency access, while the WAL preserves enough history to reconstruct the MemStore if the process restarts.

## Crash Recovery Flow
On startup, the process **replays the WAL into the MemStore**:

1. Open the WAL file.
2. Read entries sequentially from the beginning.
3. Apply each entry to the MemStore to rebuild in-memory state.

This replay step ensures the MemStore reflects all durable WAL entries before the system begins serving new requests.

## Durability vs. Performance
The WAL is written with `flush()` calls but **does not call `fsync()`**. This means:

- **Performance is higher** because the system avoids forcing disk synchronization on every write.
- **Durability is weaker** because OS buffers may still be lost on a sudden crash or power loss.

If you need stronger guarantees, you would add `fsync()` after writes at the cost of throughput and latency.

## Example Usage
While usage can be embedded in a larger application, an example flow typically looks like:

1. Start the ingestor process (optionally via a CLI wrapper).
2. Send metric updates; each update is appended to the WAL and reflected in the MemStore.
3. Restart the process to observe WAL replay restoring in-memory state.

## File Structure
```
.
├── README.md
└── src
    └── (source files)
```

## API Overview
The core API revolves around:

- **Appending metrics to the WAL** for durability.
- **Applying updates to the MemStore** for low-latency reads.
- **Replaying the WAL** at startup to rebuild the MemStore.

This separation makes it easy to reason about performance (MemStore) and recovery (WAL) while keeping the implementation straightforward.
