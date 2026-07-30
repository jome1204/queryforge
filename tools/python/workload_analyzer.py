#!/usr/bin/env python3
"""Static workload analysis for QueryForge SQL scripts.

The analyzer is intentionally conservative: it never executes SQL.  It splits
statements while respecting strings/comments, identifies common operations, and
reports schema dependencies, transaction balance, and complexity indicators.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path
from typing import Iterable, Iterator

MAX_INPUT_BYTES = 32 * 1024 * 1024
MAX_STATEMENTS = 100_000
MAX_TOKENS = 2_000_000


@dataclasses.dataclass(frozen=True)
class Token:
    kind: str
    text: str
    offset: int


@dataclasses.dataclass
class StatementInfo:
    ordinal: int
    operation: str
    tables_read: list[str]
    tables_written: list[str]
    indexes: list[str]
    token_count: int
    nesting_depth: int
    has_subquery: bool
    has_join: bool
    has_aggregate: bool
    text: str


KEYWORDS = {
    "SELECT", "FROM", "WHERE", "JOIN", "INNER", "LEFT", "RIGHT", "FULL",
    "ON", "GROUP", "BY", "HAVING", "ORDER", "LIMIT", "OFFSET", "DISTINCT",
    "INSERT", "INTO", "VALUES", "UPDATE", "SET", "DELETE", "CREATE", "TABLE",
    "INDEX", "UNIQUE", "DROP", "ALTER", "BEGIN", "COMMIT", "ROLLBACK",
    "TRANSACTION", "CHECKPOINT", "AS", "AND", "OR", "NOT", "NULL", "IS",
}
AGGREGATES = {"COUNT", "SUM", "AVG", "MIN", "MAX"}


def tokenize(sql: str) -> Iterator[Token]:
    position = 0
    produced = 0
    while position < len(sql):
        if produced >= MAX_TOKENS:
            raise ValueError("token count exceeds resource limit")
        character = sql[position]
        if character.isspace():
            position += 1
            continue
        if sql.startswith("--", position):
            end = sql.find("\n", position + 2)
            position = len(sql) if end < 0 else end + 1
            continue
        if sql.startswith("/*", position):
            end = sql.find("*/", position + 2)
            if end < 0:
                raise ValueError(f"unterminated comment at offset {position}")
            position = end + 2
            continue
        start = position
        if character in "'\"`[":
            terminator = "]" if character == "[" else character
            position += 1
            while position < len(sql):
                if sql[position] == terminator:
                    if terminator != "]" and position + 1 < len(sql):
                        if sql[position + 1] == terminator:
                            position += 2
                            continue
                    position += 1
                    break
                position += 1
            else:
                raise ValueError(f"unterminated quoted token at offset {start}")
            kind = "string" if character == "'" else "identifier"
            yield Token(kind, sql[start:position], start)
            produced += 1
            continue
        if character.isalpha() or character == "_":
            position += 1
            while position < len(sql):
                if not (sql[position].isalnum() or sql[position] in "_$"):
                    break
                position += 1
            text = sql[start:position]
            kind = "keyword" if text.upper() in KEYWORDS else "identifier"
            yield Token(kind, text, start)
            produced += 1
            continue
        if character.isdigit():
            position += 1
            while position < len(sql) and (
                sql[position].isalnum() or sql[position] in ".xX_+-"
            ):
                if sql[position] in "+-" and sql[position - 1] not in "eE":
                    break
                position += 1
            yield Token("number", sql[start:position], start)
            produced += 1
            continue
        two = sql[position : position + 2]
        if two in {"<=", ">=", "<>", "!=", "||", "=="}:
            position += 2
            yield Token("operator", two, start)
        else:
            position += 1
            kind = "punctuation" if character in "(),.;" else "operator"
            yield Token(kind, character, start)
        produced += 1


def split_statements(sql: str) -> list[str]:
    statements: list[str] = []
    start = 0
    position = 0
    quote = ""
    block_comment = False
    line_comment = False
    while position < len(sql):
        if line_comment:
            if sql[position] == "\n":
                line_comment = False
            position += 1
            continue
        if block_comment:
            if sql.startswith("*/", position):
                block_comment = False
                position += 2
            else:
                position += 1
            continue
        if quote:
            terminator = "]" if quote == "[" else quote
            if sql[position] == terminator:
                if terminator != "]" and sql.startswith(terminator * 2, position):
                    position += 2
                    continue
                quote = ""
            position += 1
            continue
        if sql.startswith("--", position):
            line_comment = True
            position += 2
            continue
        if sql.startswith("/*", position):
            block_comment = True
            position += 2
            continue
        if sql[position] in "'\"`[":
            quote = sql[position]
            position += 1
            continue
        if sql[position] == ";":
            statement = sql[start:position].strip()
            if statement:
                statements.append(statement)
                if len(statements) > MAX_STATEMENTS:
                    raise ValueError("statement count exceeds resource limit")
            start = position + 1
        position += 1
    if quote:
        raise ValueError("unterminated quoted token")
    if block_comment:
        raise ValueError("unterminated block comment")
    tail = sql[start:].strip()
    if tail:
        statements.append(tail)
    return statements


def _unquote_identifier(text: str) -> str:
    if len(text) >= 2 and text[0] == "[" and text[-1] == "]":
        return text[1:-1]
    if len(text) >= 2 and text[0] in '"`' and text[-1] == text[0]:
        return text[1:-1].replace(text[0] * 2, text[0])
    return text


def _identifier_after(tokens: list[Token], keyword_index: int) -> str | None:
    index = keyword_index + 1
    while index < len(tokens) and tokens[index].text.upper() in {
        "IF", "NOT", "EXISTS", "ONLY"
    }:
        index += 1
    if index < len(tokens) and tokens[index].kind in {"identifier", "keyword"}:
        parts = [_unquote_identifier(tokens[index].text)]
        if index + 2 < len(tokens) and tokens[index + 1].text == ".":
            parts.append(_unquote_identifier(tokens[index + 2].text))
        return ".".join(parts)
    return None


def analyze_statement(ordinal: int, statement: str) -> StatementInfo:
    tokens = list(tokenize(statement))
    upper = [token.text.upper() for token in tokens]
    operation = upper[0] if upper else "EMPTY"
    if operation == "CREATE" and len(upper) > 1:
        operation = f"CREATE {upper[1]}"
    elif operation == "DROP" and len(upper) > 1:
        operation = f"DROP {upper[1]}"

    reads: set[str] = set()
    writes: set[str] = set()
    indexes: set[str] = set()
    for index, word in enumerate(upper):
        if word in {"FROM", "JOIN"}:
            name = _identifier_after(tokens, index)
            if name:
                reads.add(name)
        if word == "UPDATE":
            name = _identifier_after(tokens, index)
            if name:
                writes.add(name)
        if word == "INTO" and index > 0 and upper[index - 1] == "INSERT":
            name = _identifier_after(tokens, index)
            if name:
                writes.add(name)
        if word == "TABLE" and index > 0 and upper[index - 1] in {"CREATE", "DROP"}:
            name = _identifier_after(tokens, index)
            if name:
                writes.add(name)
        if word == "INDEX" and index > 0 and upper[index - 1] in {
            "CREATE", "UNIQUE", "DROP"
        }:
            name = _identifier_after(tokens, index)
            if name:
                indexes.add(name)
        if word == "ON" and operation.startswith("CREATE INDEX"):
            name = _identifier_after(tokens, index)
            if name:
                writes.add(name)

    if operation == "DELETE":
        writes.update(reads)
        reads.clear()

    depth = 0
    maximum_depth = 0
    select_count = 0
    for token in tokens:
        if token.text == "(":
            depth += 1
            maximum_depth = max(maximum_depth, depth)
        elif token.text == ")":
            depth = max(0, depth - 1)
        elif token.text.upper() == "SELECT":
            select_count += 1
    return StatementInfo(
        ordinal=ordinal,
        operation=operation,
        tables_read=sorted(reads, key=str.casefold),
        tables_written=sorted(writes, key=str.casefold),
        indexes=sorted(indexes, key=str.casefold),
        token_count=len(tokens),
        nesting_depth=maximum_depth,
        has_subquery=select_count > 1,
        has_join="JOIN" in upper,
        has_aggregate=any(word in AGGREGATES for word in upper),
        text=statement,
    )


def analyze_workload(sql: str) -> dict[str, object]:
    statements = [
        analyze_statement(index + 1, text)
        for index, text in enumerate(split_statements(sql))
    ]
    operation_counts = Counter(item.operation for item in statements)
    reads: Counter[str] = Counter()
    writes: Counter[str] = Counter()
    indexes: Counter[str] = Counter()
    transaction_depth = 0
    maximum_transaction_depth = 0
    transaction_warnings: list[str] = []
    for item in statements:
        reads.update(item.tables_read)
        writes.update(item.tables_written)
        indexes.update(item.indexes)
        if item.operation == "BEGIN":
            transaction_depth += 1
            maximum_transaction_depth = max(maximum_transaction_depth, transaction_depth)
            if transaction_depth > 1:
                transaction_warnings.append(
                    f"statement {item.ordinal}: nested BEGIN"
                )
        elif item.operation in {"COMMIT", "ROLLBACK"}:
            if transaction_depth == 0:
                transaction_warnings.append(
                    f"statement {item.ordinal}: {item.operation} without BEGIN"
                )
            else:
                transaction_depth -= 1
    if transaction_depth:
        transaction_warnings.append(
            f"workload ends with {transaction_depth} open transaction(s)"
        )

    dependencies: dict[str, dict[str, int]] = defaultdict(
        lambda: {"reads": 0, "writes": 0}
    )
    for table, count in reads.items():
        dependencies[table]["reads"] += count
    for table, count in writes.items():
        dependencies[table]["writes"] += count

    return {
        "bytes": len(sql.encode("utf-8")),
        "statements": len(statements),
        "tokens": sum(item.token_count for item in statements),
        "operation_counts": dict(sorted(operation_counts.items())),
        "maximum_nesting_depth": max(
            (item.nesting_depth for item in statements), default=0
        ),
        "maximum_transaction_depth": maximum_transaction_depth,
        "transaction_warnings": transaction_warnings,
        "features": {
            "joins": sum(item.has_join for item in statements),
            "subqueries": sum(item.has_subquery for item in statements),
            "aggregates": sum(item.has_aggregate for item in statements),
        },
        "table_dependencies": dict(sorted(dependencies.items())),
        "indexes": dict(sorted(indexes.items())),
        "statement_details": [dataclasses.asdict(item) for item in statements],
    }


def render_text(report: dict[str, object], verbose: bool) -> str:
    lines = [
        f"Statements: {report['statements']}",
        f"Tokens: {report['tokens']}",
        f"Maximum expression nesting: {report['maximum_nesting_depth']}",
        f"Maximum transaction nesting: {report['maximum_transaction_depth']}",
        "Operations:",
    ]
    for operation, count in report["operation_counts"].items():  # type: ignore[union-attr]
        lines.append(f"  {operation}: {count}")
    lines.append("Tables:")
    for table, usage in report["table_dependencies"].items():  # type: ignore[union-attr]
        lines.append(
            f"  {table}: {usage['reads']} reads, {usage['writes']} writes"
        )
    warnings = report["transaction_warnings"]
    if warnings:
        lines.append("Warnings:")
        lines.extend(f"  {warning}" for warning in warnings)  # type: ignore[arg-type]
    if verbose:
        lines.append("Statements:")
        for item in report["statement_details"]:  # type: ignore[assignment]
            lines.append(
                f"  {item['ordinal']:4}: {item['operation']:<14} "
                f"tokens={item['token_count']:<4} depth={item['nesting_depth']}"
            )
    return "\n".join(lines)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", type=Path, nargs="+")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    results: dict[str, object] = {}
    try:
        for path in arguments.files:
            if path.stat().st_size > MAX_INPUT_BYTES:
                raise ValueError(f"{path}: input exceeds resource limit")
            sql = path.read_text(encoding="utf-8")
            results[str(path)] = analyze_workload(sql)
        if arguments.json:
            print(json.dumps(results, indent=2, ensure_ascii=False))
        else:
            for index, (name, report) in enumerate(results.items()):
                if index:
                    print()
                print(f"{name}:")
                print(render_text(report, arguments.verbose))
        return 0
    except (OSError, UnicodeError, ValueError) as error:
        print(f"workload_analyzer: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
