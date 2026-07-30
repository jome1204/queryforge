#!/usr/bin/env python3
"""Tokenize, split, format, and measure SQL without external packages."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from dataclasses import asdict, dataclass
from typing import Iterable

MAX_SQL = 8 * 1024 * 1024
MAX_TOKENS = 524_288
KEYWORDS = {
    "SELECT", "FROM", "WHERE", "INSERT", "INTO", "VALUES", "UPDATE", "SET",
    "DELETE", "CREATE", "TABLE", "INDEX", "UNIQUE", "ON", "DROP", "AND",
    "OR", "NOT", "NULL", "TRUE", "FALSE", "AS", "ORDER", "BY", "ASC",
    "DESC", "LIMIT", "OFFSET", "JOIN", "INNER", "LEFT", "BEGIN", "COMMIT",
    "ROLLBACK", "CHECKPOINT", "PRIMARY", "KEY", "DEFAULT", "INTEGER", "REAL",
    "TEXT", "BOOLEAN", "BLOB", "IS", "IN", "LIKE", "GROUP", "HAVING",
}


class SqlError(ValueError):
    pass


@dataclass
class Token:
    kind: str
    text: str
    offset: int
    length: int


def tokenize(sql: str) -> list[Token]:
    if len(sql.encode()) > MAX_SQL:
        raise SqlError("SQL input exceeds byte limit")
    output: list[Token] = []
    position = 0
    while position < len(sql):
        character = sql[position]
        if character.isspace():
            position += 1
            continue
        if sql.startswith("--", position):
            newline = sql.find("\n", position + 2)
            position = len(sql) if newline < 0 else newline + 1
            continue
        if sql.startswith("/*", position):
            begin, depth = position, 1
            position += 2
            while position < len(sql) and depth:
                if sql.startswith("/*", position):
                    depth += 1
                    if depth > 128:
                        raise SqlError("comment nesting exceeds limit")
                    position += 2
                elif sql.startswith("*/", position):
                    depth -= 1
                    position += 2
                else:
                    position += 1
            if depth:
                raise SqlError(f"unterminated comment at {begin}")
            continue
        begin = position
        if character.isalpha() or character == "_":
            position += 1
            while position < len(sql) and \
                    (sql[position].isalnum() or sql[position] == "_"):
                position += 1
            text = sql[begin:position]
            kind = "keyword" if text.upper() in KEYWORDS else "identifier"
            output.append(Token(kind, text, begin, position - begin))
        elif character.isdigit():
            pattern = re.compile(r"(?:\d+\.\d*|\d+)(?:[eE][+-]?\d+)?")
            match = pattern.match(sql, position)
            assert match is not None
            position = match.end()
            text = match.group()
            output.append(Token("real" if any(c in text for c in ".eE")
                                else "integer",
                                text, begin, position - begin))
        elif character in "'\"`[":
            closing = "]" if character == "[" else character
            position += 1
            value: list[str] = []
            while position < len(sql):
                current = sql[position]
                position += 1
                if current == closing:
                    if position < len(sql) and sql[position] == closing and \
                            character != "[":
                        value.append(closing)
                        position += 1
                    else:
                        break
                else:
                    value.append(current)
                if len(value) > 1024 * 1024:
                    raise SqlError("quoted token exceeds limit")
            else:
                raise SqlError(f"unterminated quote at {begin}")
            output.append(Token("string" if character == "'" else "identifier",
                                "".join(value), begin, position - begin))
        else:
            pair = sql[position:position + 2]
            if pair in ("!=", "<=", ">=", "<>"):
                position += 2
                output.append(Token("operator", pair, begin, 2))
            elif character in ",.;()*+-/%=<>":
                position += 1
                kind = "punctuation" if character in ",.;()" else "operator"
                output.append(Token(kind, character, begin, 1))
            else:
                raise SqlError(f"unexpected character at {position}")
        if len(output) > MAX_TOKENS:
            raise SqlError("token count exceeds limit")
    return output


def split(sql: str, tokens: list[Token]) -> list[str]:
    output: list[str] = []
    begin = 0
    for token in tokens:
        if token.text != ";":
            continue
        end = token.offset + token.length
        statement = sql[begin:end].strip()
        if statement:
            output.append(statement)
        begin = end
    tail = sql[begin:].strip()
    if tail:
        output.append(tail)
    if len(output) > 4096:
        raise SqlError("statement count exceeds limit")
    return output


def format_tokens(tokens: list[Token]) -> str:
    output: list[str] = []
    depth = 0
    line_start = True
    newline_words = {"SELECT", "FROM", "WHERE", "GROUP", "HAVING", "ORDER",
                     "LIMIT", "JOIN", "LEFT", "INNER", "INSERT", "UPDATE",
                     "DELETE", "CREATE", "DROP", "BEGIN", "COMMIT",
                     "ROLLBACK", "CHECKPOINT"}
    previous = ""
    for token in tokens:
        text = token.text.upper() if token.kind == "keyword" else token.text
        if token.kind == "string":
            text = "'" + token.text.replace("'", "''") + "'"
        if text == ")":
            depth = max(0, depth - 1)
        if text in newline_words and output and not line_start:
            output.append("\n")
            line_start = True
        if line_start:
            output.append("  " * depth)
            line_start = False
        needs_space = (output and not output[-1].endswith(("\n", " ")) and
                       text not in (",", ")", ".", ";") and
                       previous not in ("(", "."))
        if needs_space:
            output.append(" ")
        output.append(text)
        if text == ",":
            output.append(" ")
        if text == ";":
            output.append("\n")
            line_start = True
        if text == "(":
            depth += 1
        previous = text
    return "".join(output).rstrip()


def metrics(tokens: list[Token], statements: list[str]) -> dict[str, object]:
    counts: dict[str, int] = {}
    maximum_parentheses = depth = 0
    for token in tokens:
        counts[token.kind] = counts.get(token.kind, 0) + 1
        if token.text == "(":
            depth += 1
            maximum_parentheses = max(maximum_parentheses, depth)
        elif token.text == ")":
            depth = max(0, depth - 1)
    return {"token_count": len(tokens), "statement_count": len(statements),
            "token_kinds": counts,
            "maximum_parenthesis_depth": maximum_parentheses,
            "keywords": sorted({token.text.upper() for token in tokens
                                if token.kind == "keyword"})}


def main(arguments: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sql", type=pathlib.Path)
    parser.add_argument("--format", action="store_true")
    parser.add_argument("--tokens", action="store_true")
    options = parser.parse_args(arguments)
    try:
        sql = options.sql.read_text(encoding="utf-8")
        tokens = tokenize(sql)
        statements = split(sql, tokens)
        report = metrics(tokens, statements)
        report["statements"] = statements
        if options.tokens:
            report["tokens"] = [asdict(token) for token in tokens]
        print(json.dumps(report, indent=2, sort_keys=True))
        if options.format:
            print("\n-- formatted --\n" + format_tokens(tokens))
    except (OSError, UnicodeError, SqlError) as error:
        print(f"sql_tool: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
