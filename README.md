# QueryForge

QueryForge is an original, dependency-free embedded relational database written
in C++17 for hostile-input processing and sanitizer-guided fuzzing. It includes
a bounded SQL lexer and recursive-descent parser, typed expressions, semantic
analysis, query plans, table scans, filtering, projection, sorting, joins,
aggregation, page-oriented persistence, B-tree indexes, transactions,
write-ahead logging, checkpoints, and recovery.

Portable C, Python, and Java inspection tools independently validate database
pages and WAL records. They are not build dependencies. Configuration,
compilation, testing, and fuzzing are fully offline.

## Build

```sh
cmake -S . -B build -DQUERYFORGE_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

ClusterFuzzLite compiles all five harnesses to `$OUT`:

- `sql_parser_fuzzer`
- `sql_execution_fuzzer`
- `database_file_fuzzer`
- `wal_recovery_fuzzer`
- `transaction_sequence_fuzzer`

Every harness owns a separate structured corpus under `fuzz/corpus/`.

## Safety model

Parser nesting, statement counts, identifiers, strings, rows, columns, pages,
record lengths, index fanout, transaction undo entries, WAL records, query
steps, and output rows are bounded before allocation or traversal. Page
arithmetic and record offsets use checked integer operations. Transactions own
value copies rather than pointers into mutable schema objects. Recovery validates
checksums and page generations before applying records.

Copyright (c) 2026. All rights reserved. This private repository may not be
redistributed or published.
