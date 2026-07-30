#!/bin/bash
set -euxo pipefail

cd "$SRC/queryforge"
mkdir -p "$WORK/obj" "$OUT"

sources=(
  src/analysis.cc
  src/btree.cc
  src/catalog.cc
  src/database.cc
  src/expression.cc
  src/lexer.cc
  src/page.cc
  src/parser.cc
  src/planner.cc
  src/record.cc
  src/transaction.cc
  src/utilities.cc
  src/value.cc
  src/wal.cc
)

objects=()
for source in "${sources[@]}"; do
  object="$WORK/obj/$(basename "${source%.cc}").o"
  "$CXX" $CXXFLAGS -std=c++17 -I"$SRC/queryforge/include" \
    -c "$SRC/queryforge/$source" -o "$object"
  objects+=("$object")
done

targets=(
  sql_parser_fuzzer
  sql_execution_fuzzer
  database_file_fuzzer
  wal_recovery_fuzzer
  transaction_sequence_fuzzer
)

for target in "${targets[@]}"; do
  "$CXX" $CXXFLAGS -std=c++17 -I"$SRC/queryforge/include" \
    "$SRC/queryforge/fuzz/$target.cc" "${objects[@]}" \
    "$LIB_FUZZING_ENGINE" -o "$OUT/$target"
done
