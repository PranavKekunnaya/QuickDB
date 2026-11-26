# QuickDB: project and interview guide

## 30-second explanation

QuickDB is a C++17 command-line database that implements typed tables, inserts,
and equality queries. I can compare a full scan with a nonunique hash index on
the same data. Mutations are written and synced to a write-ahead log before they
are applied. On restart, the engine replays the log to reconstruct tables and
indexes. I tested recovery after a forced process exit, incomplete final records,
and checksum corruption, as well as normal query correctness.

Use this description after reviewing and running the implementation yourself.
Describe the project as an educational prototype and be clear about assistance
used during development; do not claim production users or performance you have
not measured.

## Read the implementation in this order

1. `lex` and `Parser`: tokenize quoted text and validate command grammar.
2. `typed`: require quoted TEXT and canonicalize signed 64-bit INT values.
3. `Table`: schema, vector of rows, and per-column hash indexes of row IDs.
4. `Database::execute`: validate an entire mutation, log it, then apply it.
5. `Database::log`: frame the command, handle partial writes, and call fsync.
6. `Database::recover`: validate records, replay commands, repair incomplete tail.
7. `tests/test_database.py`: black-box invariants and process-failure scenarios.
8. `benchmarks/benchmark.py`: identical query workloads and correctness checks.

## Data flow

A command becomes tokens, then a validated operation. CREATE and INSERT are
logged before changing the in-memory database. CREATE INDEX builds a temporary
index before logging and installing it. SELECT chooses an index if its equality
column has one, unless SCAN is specified. It returns matching rows and an
execution summary. EXPORT serializes the current table to CSV. At startup, replay
uses the same parser and mutation logic with logging and output disabled.

## Interview questions and answer outlines

### Why a hash index instead of a B+ tree?

My supported predicate is equality. A hash map gives expected constant-time key
lookup and a vector preserves multiple rows with the same key. A B+ tree would
be more appropriate for ranges and ordered scans, at the cost of a more complex
implementation. I did not implement a B+ tree.

### What does the index contain?

Each indexed column maps a canonical string value to a vector of row offsets.
The table stores the actual row once. On insertion I append its row ID to each
relevant index. Rows are append-only, so those offsets remain stable.

### What is the time and space complexity?

A scan is O(n); an equality hash lookup is expected O(1+k), with O(n) worst-case
hash behavior and k matching rows. Building an index is expected O(n), with O(n)
additional storage. Rebuilding the database requires replaying all retained
mutations and rebuilding index state.

### Why normalize integers?

Without normalization, `1` and `+001` would have different string hash keys even
though they represent the same integer. Converting through a checked signed
64-bit value makes scan and index comparisons consistent.

### What does the WAL guarantee?

A mutation is validated, appended, and synced before application and success
acknowledgment. A complete valid record is replayable. If the process dies after
sync but before application, replay applies it on restart. This is a single
mutation commit model, not full multi-statement ACID transactions.

### What happens if the caller never gets a response?

The operation may already be committed. A retry can insert a duplicate. A more
complete design would add request IDs, uniqueness constraints, or application
idempotency. The current engine does not solve exactly-once client delivery.

### How do you detect a partial write?

Records contain payload length, checksum, and a terminator. EOF inside the final
header or payload indicates an incomplete tail that can be discarded. A complete
record with a checksum mismatch is treated as corruption and causes failure;
recovery does not silently discard valid-looking history after corruption.

### Is this safe against all power failures?

No such claim was tested. fsync requests durability from the OS, and the database
directory is synced, but filesystem/device behavior still matters and the parent
of a newly created database directory is not synced. The tests simulate process
failure and torn log bytes. They do not reproduce hardware power loss.

### Why use a checksum, and is it secure?

The checksum catches common accidental byte changes. FNV-1a is small and easy to
implement, but it is noncryptographic and collisions exist. An adversarial threat
model would need authentication, stronger integrity mechanisms, and permissions
beyond this project.

### How did you prove the index is correct?

I compare indexed and forced-scan outputs for the same keys, including duplicate
keys and inserts after index creation. I also restart the process and verify the
index is rebuilt. The benchmark verifies output equivalence before accepting
timings; speed without identical results is not useful.

### What does the benchmark measure?

Only in-process matching and collection of result row IDs. It excludes parsing,
printing, IPC, and fsync. The workload has unique integer keys at three table
sizes, fixed random seeds, warmup queries, alternating execution order, and 500
measured queries per plan per size. I report median and p99, with no claim about
real application throughput or memory savings.

### Why not benchmark inserts the same way?

Inserts include a disk sync, unlike the in-memory lookup path. That needs a
separate workload with filesystem details, durability settings, and wall-clock
measurements. Mixing those measurements would obscure what the index improves.

### What was the main engineering tradeoff?

I chose a simple append-only WAL and in-memory state so recovery could be tested
clearly. That makes startup proportional to historical writes and uses memory
proportional to data plus indexes. Checkpoints, compaction, and disk pages are
natural next steps but are not implemented.

### What would you improve next?

First add checkpoints with atomic replacement and durable directory updates,
then failure-injection tests at every checkpoint stage. After that, implement
range queries with a B+ tree or introduce a page cache. Concurrency and grouped
transactions require explicit locking and commit semantics, not just threads.

### How is CSV used?

CSV is an export format for inspection and downstream analysis. The authoritative
format is the WAL, which includes schema and mutation history. I would not claim
that CSV exports are a transactional storage engine or automatically imported.

### How would you describe results on a resume?

- Built a C++17 database engine with typed schemas, inserts, equality queries,
  and CSV export to support structured record storage and retrieval.
- Implemented nonunique hash indexes and a forced-scan baseline; verified equal
  results across 1k–50k-row workloads while measuring median and p99 lookup time.
- Added synced write-ahead logging and startup replay; passed eight integration
  tests covering query correctness, process crashes, torn writes, and corruption.

Add numerical speed comparisons only after inspecting the committed benchmark
results and explaining their narrow measurement scope.
