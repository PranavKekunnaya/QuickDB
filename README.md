# QuickDB

A small C++17 database engine built to explore parsing, typed rows, hash indexes,
and write-ahead-log recovery. It runs as a Linux command-line program with no
third-party C++ dependencies. This is an educational database, not a production
replacement for SQLite or PostgreSQL.

## Build and run

```bash
mkdir -p build
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic src/main.cpp -o build/quickdb
./build/quickdb ./demo-db
```

Alternatively, with CMake 3.16+ and Python 3 installed:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Enter one command per line. Keywords and types must be uppercase; identifiers
are case-sensitive. Semicolons are optional. TEXT literals use single quotes;
escape an embedded quote by doubling it. INT uses signed 64-bit values and
normalizes representations such as `+001` to `1`.

```sql
CREATE TABLE students (id INT, name TEXT);
INSERT INTO students VALUES (1, 'Ada');
INSERT INTO students VALUES (2, 'Grace');
CREATE INDEX ON students (id);
SELECT * FROM students WHERE id = 2;
SELECT * FROM students WHERE id = 2 SCAN;
SELECT * FROM students;
EXPORT students;
QUIT
```

`SCAN` forces the baseline scan even when an index exists. SELECT prints CSV
rows followed by a summary reporting matches, rows examined, query plan, and
execution time. `EXPORT students` writes `demo-db/students.csv`, including a
header. Exported CSV is a convenience output, **not authoritative storage**.

## What is implemented

- CREATE TABLE with INT/TEXT schemas, duplicate-column checks, and input validation.
- INSERT with arity checking, integer range checking, and quoted TEXT parsing.
- SELECT all rows or one equality predicate; scan and hash-index execution paths.
- Nonunique secondary indexes that retain all matches and update on insertion.
- A length-framed, checksummed WAL containing validated mutations and schemas.
- Log append and `fsync` before applying a mutation or reporting success.
- Startup replay that rebuilds rows and indexes; incomplete final records are
  truncated, while complete corrupt records cause startup to fail.
- An exclusive process lock preventing two processes from opening one database.
- CSV export, integration tests, and a reproducible scan/index benchmark.

## Storage and recovery

The WAL is append-only during normal operation. Each record contains a decimal
byte length, a 32-bit FNV-1a checksum, a newline, the original command, and a
final newline. A complete validated record is the commit unit. A crash after
logging but before the response can therefore leave an operation committed even
though the client never received `OK`—clients cannot blindly retry inserts.

Recovery starts from empty state and replays the full WAL. There is no checkpoint,
log compaction, transaction grouping, rollback, or page cache. All rows and indexes
reside in memory. CSV exports do not participate in recovery. Existing CSV-backed
QuickDB files from other implementations are not automatically imported.

The implementation syncs the WAL and its containing directory. Durability still
depends on OS/filesystem/device guarantees; a newly created database directory's
parent is not explicitly synced. The tests exercise process crashes and torn
records, not physical power failures. FNV-1a detects common accidental corruption;
it is not cryptographic protection or a guarantee against every possible fault.

## Test

```bash
python3 tests/test_database.py build/quickdb
```

Eight tests cover validation without mutation, scan/index equivalence with
nonunique keys, index reconstruction after restart, CSV escaping, a process exit
after durable logging, incomplete-tail recovery, corruption rejection, and
exclusive opening. The `QUICKDB_CRASH_AFTER_WAL=1` environment variable is a test
hook that exits with status 86 after syncing the next mutation, before applying it.

## Benchmark

```bash
python3 benchmarks/benchmark.py build/quickdb
```

See [results.csv](benchmarks/results.csv) for measurements from the development
environment. For 1k, 10k, and 50k rows, the script uses identical deterministic
keys for scan and index queries, verifies identical returned rows, alternates
execution order, warms up with 50 query pairs, and measures 500 pairs. Timing
covers matching and result-ID collection, excluding parsing, output, IPC, and
storage sync. These are in-memory point-lookup measurements, not end-to-end
throughput or production latency claims. Nanosecond-scale index results are
sensitive to clock overhead and system noise. Memory usage is not benchmarked.

## Tradeoffs and boundaries

Expected equality lookup is O(1 + k) with a hash index versus O(n) for a scan;
worst-case hash behavior is O(n). Index construction takes expected O(n) time
and O(n) additional space. `k` is the number of matches. Indexes cannot accelerate
range predicates, ordering, or joins (which are unsupported). INSERT performs a
sync per mutation, intentionally favoring a clear persistence contract over write
throughput. Recovery cost grows with total WAL history.

No UPDATE/DELETE, SQL joins, range predicates, NULL, uniqueness constraints,
concurrent readers, multi-statement transactions, network server, or Windows
support. Input commands are limited to 1 MiB and cannot contain embedded newlines.

