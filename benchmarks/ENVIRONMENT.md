# Recorded benchmark environment

- Linux 6.18.57, x86_64; glibc 2.41.
- GCC 14.2.0; `g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic`.
- Shared development container; CPU allocation and background load uncontrolled.
- 50 warmup pairs followed by 500 measured query pairs per table size.
- Query execution timing only; see README for exclusions and limitations.
- Results are a single run, not an aggregate across machines or repeated trials.

At 50,000 rows, median matching time was 225,813 ns for the scan and 769.5 ns for
the hash-index path in this run. This comparison excludes SQL parsing and I/O;
it should not be presented as an end-to-end application speedup.
